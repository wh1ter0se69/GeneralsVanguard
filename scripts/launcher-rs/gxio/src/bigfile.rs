//! Reading one file out of the game's mounted .big archives.
//!
//! WHY THE LAUNCHER NEEDS THIS. The Keybinds screen has to say what each structure builds, and
//! that lives in `Data\INI\CommandSet\*.ini` and `Data\INI\CommandButton.ini`. Neither exists as a
//! loose file on a normal install - they are inside the archives, and nothing in the launcher had
//! ever opened one before.
//!
//! # The container
//!
//! ```text
//! 0x00  4      "BIGF"
//! 0x04  4  LE  total archive size          (measured == st_size on all 36 archives here)
//! 0x08  4  BE  entry count
//! 0x0C  4  BE  offset of the first data byte  - the engine never reads this; neither do we
//! 0x10  ...    entry table, packed, no padding:
//!                4 BE  data offset
//!                4 BE  data length
//!                var   NUL-terminated name, single-byte chars, '\' separators
//! ```
//!
//! The mixed endianness is the format, not a bug: `StdBIGFileSystem.cpp:1040` reads the size with
//! no `betoh` while `:1048` byte-swaps the count.
//!
//! Names are BYTES, not UTF-8. They are decoded as latin-1 so that a mod archive with a high byte
//! in a filename cannot make the whole archive unreadable. Lookup is case-insensitive and
//! separator-agnostic, matching `ArchiveFileSystem.cpp:132` (`toLower` then tokenise on `"\\/"`).
//!
//! # Load order is the whole game here, and getting it backwards is silent
//!
//! The engine keeps its filename list in a `std::set<AsciiString, less_than_nocase>`
//! (`FileSystem.h:66`), mounts in that order, and `loadIntoDirectoryTree(overwrite=FALSE)` APPENDS
//! - so resolution takes **instance 0**, the earliest-mounted archive. That is why every community
//! override is named `!…`, `340_…`, `500_900_…`: `'!'` is 0x21 and sorts before every digit and
//! letter.
//!
//! The patch overlay is NOT simply appended after the game folder.
//! `StdBIGFileSystem::loadIntoDirectoryTreeAtPriority` inserts each overlay archive at the position
//! its own basename earns among the already-mounted ones, and its comparison is `<= 0`, so on an
//! exact basename tie the game-folder copy keeps precedence. `mount_order_of` reproduces exactly
//! that: measured on this Mac the eight duplicated basenames all resolve to the game-folder copy.
//!
//! # A zero-byte entry is a HIT, not a miss
//!
//! The Windows loader's `BigReader.Read` skips empty entries and returns the next archive's copy.
//! The engine has no such rule, and the difference changes the answer: `Data\INI\CommandSet.ini`
//! resolves to `500_900_CommunityPatch_CoreINI.big`'s **zero-byte** file, and the 472 real command
//! sets live in `Data\INI\CommandSet\*.ini`. Skip-empty hands back INIZH.big's 144,715-byte
//! monolith - data this install does not load - and the screen then shows the vanilla game.
//!
//! So [`Mount::read`] returns instance 0 verbatim, including a zero-length body, and callers that
//! want the engine's own fallback ask for instance 1 explicitly ([`Mount::instances`]).

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// Refused rather than panicked. A player's archive can be truncated by a failed download, and a
/// launcher that crashes on the settings screen is worse than one that says "could not read this".
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BigError {
    /// Under 16 bytes: there is not even a header.
    TooSmall,
    /// The first four bytes are not `BIGF`.
    NotBig,
    /// Zero, or past the sanity clamp. A corrupt count would otherwise drive a multi-million
    /// iteration loop over a small file.
    ImplausibleCount(u32),
    /// The entry table ran off the end of the file, or a name had no terminator inside a sane
    /// length. Either way the table cannot be trusted.
    TruncatedTable,
    /// An entry claims bytes the file does not have.
    EntryOutOfBounds { name: String },
    Io(String),
}

impl std::fmt::Display for BigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BigError::TooSmall => write!(f, "not a .big file: shorter than its own header"),
            BigError::NotBig => write!(f, "not a .big file: no BIGF signature"),
            BigError::ImplausibleCount(n) => write!(f, "implausible entry count {n}"),
            BigError::TruncatedTable => write!(f, "the entry table is truncated"),
            BigError::EntryOutOfBounds { name } => {
                write!(f, "entry {name} claims bytes past the end of the archive")
            }
            BigError::Io(e) => write!(f, "{e}"),
        }
    }
}

/// The largest entry count worth believing. The biggest archive on a normal install carries about
/// 4,400 entries; 200,000 is the Windows loader's clamp and is kept for the same reason.
const MAX_ENTRIES: u32 = 200_000;

/// No archive name is this long. A table with no NUL inside this window is unreadable, not a very
/// long filename.
const MAX_NAME: usize = 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// As stored, latin-1 decoded, backslashes intact.
    pub name: String,
    /// The lookup key: `/` folded to `\`, leading separators stripped, lowercased.
    pub key: String,
    pub offset: u32,
    pub len: u32,
}

/// The lookup key for an entry or a caller's path. Callers should not have to care which slash
/// they typed or how the archive spelled its case.
pub fn normalise(name: &str) -> String {
    let mut s = String::with_capacity(name.len());
    for c in name.chars() {
        s.push(if c == '/' { '\\' } else { c });
    }
    let s = s.trim_start_matches('\\');
    s.to_ascii_lowercase()
}

fn be32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

/// Parse the header and entry table out of `bytes`, which must begin at offset 0 of the archive
/// and hold at least the whole table. PURE: no I/O, so the format is testable against crafted
/// fixtures without a game installed.
///
/// `file_len` is the real size on disk and is used only to reject entries pointing past the end.
pub fn parse_index(bytes: &[u8], file_len: u64) -> Result<Vec<Entry>, BigError> {
    if bytes.len() < 16 {
        return Err(BigError::TooSmall);
    }
    if &bytes[0..4] != b"BIGF" {
        return Err(BigError::NotBig);
    }
    // bytes[4..8] is the total size, little-endian, and is deliberately unread: the engine does not
    // check it either, and a repacked archive with a stale value is otherwise perfectly usable.
    let count = be32(&bytes[8..12]);
    if count == 0 || count > MAX_ENTRIES {
        return Err(BigError::ImplausibleCount(count));
    }

    let mut out = Vec::with_capacity(count.min(8192) as usize);
    let mut p = 0x10usize;
    for _ in 0..count {
        let Some(head) = bytes.get(p..p + 8) else {
            return Err(BigError::TruncatedTable);
        };
        let offset = be32(&head[0..4]);
        let len = be32(&head[4..8]);
        p += 8;

        let start = p;
        let mut end = None;
        let limit = (start + MAX_NAME).min(bytes.len());
        for (i, b) in bytes[start..limit].iter().enumerate() {
            if *b == 0 {
                end = Some(start + i);
                break;
            }
        }
        let Some(end) = end else {
            return Err(BigError::TruncatedTable);
        };
        // latin-1: every byte is a character. Names are bytes in this format and a high byte in a
        // mod's filename must not cost the whole archive.
        let name: String = bytes[start..end].iter().map(|b| *b as char).collect();
        p = end + 1;

        if offset as u64 > file_len || offset as u64 + len as u64 > file_len {
            return Err(BigError::EntryOutOfBounds { name });
        }
        let key = normalise(&name);
        out.push(Entry { name, key, offset, len });
    }
    Ok(out)
}

/// One archive, indexed but NOT loaded. Opening a 254 MB archive to read a 5 KB INI would be
/// absurd, so only the entry table is read and every body is fetched with a seek.
#[derive(Debug)]
pub struct Archive {
    path: PathBuf,
    file_len: u64,
    entries: Vec<Entry>,
}

impl Archive {
    pub fn open(path: &Path) -> Result<Archive, BigError> {
        let file = File::open(path).map_err(|e| BigError::Io(e.to_string()))?;
        let file_len = file
            .metadata()
            .map_err(|e| BigError::Io(e.to_string()))?
            .len();
        let mut r = BufReader::with_capacity(64 * 1024, file);

        let mut head = [0u8; 16];
        r.read_exact(&mut head).map_err(|_| BigError::TooSmall)?;
        if &head[0..4] != b"BIGF" {
            return Err(BigError::NotBig);
        }
        let count = be32(&head[8..12]);
        if count == 0 || count > MAX_ENTRIES {
            return Err(BigError::ImplausibleCount(count));
        }

        // Read the table incrementally. Its size is not stated anywhere we trust (the field at
        // 0x0C is the one the engine ignores), so grow a buffer until every entry has parsed
        // rather than believing a number a corrupt file supplied.
        let mut buf = Vec::with_capacity(16 + (count as usize).min(8192) * 32);
        buf.extend_from_slice(&head);
        let mut chunk = vec![0u8; 256 * 1024];
        loop {
            match parse_index(&buf, file_len) {
                Ok(entries) => {
                    return Ok(Archive { path: path.to_path_buf(), file_len, entries })
                }
                Err(BigError::TruncatedTable) => {
                    let n = r
                        .read(&mut chunk)
                        .map_err(|e| BigError::Io(e.to_string()))?;
                    if n == 0 {
                        return Err(BigError::TruncatedTable);
                    }
                    buf.extend_from_slice(&chunk[..n]);
                }
                Err(e) => return Err(e),
            }
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The filename alone, lowercased - the identity the engine sorts on.
    pub fn basename(&self) -> String {
        self.path
            .file_name()
            .map(|s| s.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default()
    }

    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    pub fn read_entry(&self, idx: usize) -> Result<Vec<u8>, BigError> {
        let Some(e) = self.entries.get(idx) else {
            return Err(BigError::TruncatedTable);
        };
        if e.offset as u64 + e.len as u64 > self.file_len {
            return Err(BigError::EntryOutOfBounds { name: e.name.clone() });
        }
        let mut f = File::open(&self.path).map_err(|x| BigError::Io(x.to_string()))?;
        f.seek(SeekFrom::Start(e.offset as u64))
            .map_err(|x| BigError::Io(x.to_string()))?;
        let mut out = vec![0u8; e.len as usize];
        f.read_exact(&mut out)
            .map_err(|x| BigError::Io(x.to_string()))?;
        Ok(out)
    }
}

/// The engine's mount order over a game folder and its overlay, as a list of `(path, from_overlay)`.
///
/// PURE over the two name lists so the precedence rule can be tested without a game installed.
/// The rule: the game folder sorted by lowercased basename, then each overlay archive inserted
/// before the first entry whose lowercased basename sorts STRICTLY greater. Strictly, because
/// `loadIntoDirectoryTreeAtPriority` compares with `<= 0` and therefore leaves an exact tie with
/// the copy already mounted.
pub fn mount_order_of(game: &[String], patch: &[String]) -> Vec<(String, bool)> {
    let mut game: Vec<String> = game.to_vec();
    game.sort_by_key(|n| n.to_ascii_lowercase());
    let mut order: Vec<(String, bool)> = game.into_iter().map(|n| (n, false)).collect();

    let mut patch: Vec<String> = patch.to_vec();
    patch.sort_by_key(|n| n.to_ascii_lowercase());
    for name in patch {
        let key = name.to_ascii_lowercase();
        let at = order
            .iter()
            .position(|(n, _)| n.to_ascii_lowercase() > key)
            .unwrap_or(order.len());
        order.insert(at, (name, true));
    }
    order
}

/// `.off` is how the launcher disables an optional archive on the General page. A disabled archive
/// is not mounted, so it must not appear here either or the Keybinds screen would quote data the
/// running game does not load.
fn big_names_in(dir: &Path) -> Vec<String> {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for e in rd.flatten() {
        if !e.path().is_file() {
            continue;
        }
        let name = e.file_name().to_string_lossy().to_string();
        if name.to_ascii_lowercase().ends_with(".big") {
            out.push(name);
        }
    }
    out
}

/// Every archive the game would mount, indexed, in engine order.
#[derive(Debug, Default)]
pub struct Mount {
    archives: Vec<Archive>,
    /// normalised entry key -> every (archive, entry) carrying it, in mount order.
    by_key: HashMap<String, Vec<(usize, usize)>>,
    /// Archives that exist but could not be indexed, with the reason. Reported, never fatal - one
    /// locked or half-downloaded archive must not blank the screen.
    pub skipped: Vec<(String, String)>,
}

impl Mount {
    /// `game_dir` is the folder holding INIZH.big; `patch_dir` is the overlay beside it. Either may
    /// be absent, which is an ordinary state - the game may not be installed.
    pub fn open(game_dir: Option<&Path>, patch_dir: Option<&Path>) -> Mount {
        let g = game_dir.map(big_names_in).unwrap_or_default();
        // A migrated install has the overlay in its own folder; an unmigrated one keeps the
        // archives in the game folder and gxio::game reports the SAME path for both. Mounting it
        // twice would duplicate every entry, so the overlay is dropped when it is the game folder.
        let same = match (game_dir, patch_dir) {
            (Some(a), Some(b)) => a == b,
            _ => false,
        };
        let p = if same {
            Vec::new()
        } else {
            patch_dir.map(big_names_in).unwrap_or_default()
        };

        let mut m = Mount::default();
        for (name, from_overlay) in mount_order_of(&g, &p) {
            let dir = if from_overlay { patch_dir } else { game_dir };
            let Some(dir) = dir else { continue };
            let path = dir.join(&name);
            match Archive::open(&path) {
                Ok(a) => m.archives.push(a),
                Err(e) => m.skipped.push((name, e.to_string())),
            }
        }
        for (ai, a) in m.archives.iter().enumerate() {
            for (ei, e) in a.entries.iter().enumerate() {
                m.by_key.entry(e.key.clone()).or_default().push((ai, ei));
            }
        }
        m
    }

    pub fn is_empty(&self) -> bool {
        self.archives.is_empty()
    }

    pub fn archive_count(&self) -> usize {
        self.archives.len()
    }

    /// Every archive carrying `path`, in mount order. Instance 0 is what the engine resolves;
    /// instance 1 is what `GameText`'s fallback table uses.
    pub fn instances(&self, path: &str) -> &[(usize, usize)] {
        self.by_key
            .get(&normalise(path))
            .map(|v| v.as_slice())
            .unwrap_or(&[])
    }

    /// The archive an instance came from, by lowercased basename.
    pub fn source_of(&self, inst: (usize, usize)) -> String {
        self.archives
            .get(inst.0)
            .map(|a| a.basename())
            .unwrap_or_default()
    }

    pub fn read_instance(&self, inst: (usize, usize)) -> Option<Vec<u8>> {
        self.archives.get(inst.0)?.read_entry(inst.1).ok()
    }

    /// Instance `n` of `path`. `read(p, 0)` is what the engine reads - including when it is zero
    /// bytes long.
    pub fn read(&self, path: &str, instance: usize) -> Option<Vec<u8>> {
        let v = self.instances(path);
        v.get(instance).and_then(|i| self.read_instance(*i))
    }

    /// The first instance whose archive basename is not in `avoid`, else instance 0.
    ///
    /// This is the ONE place the launcher knowingly reads different bytes than the game, and it
    /// exists for the cameos: see `commandset::INDICATOR_PACK`.
    pub fn read_avoiding(&self, path: &str, avoid: &[&str]) -> Option<(Vec<u8>, String)> {
        let v = self.instances(path);
        let pick = v
            .iter()
            .find(|i| !avoid.contains(&self.source_of(**i).as_str()))
            .or_else(|| v.first())?;
        let src = self.source_of(*pick);
        self.read_instance(*pick).map(|b| (b, src))
    }

    /// Every distinct filename under a directory prefix, first-instance-wins, sorted by key.
    ///
    /// `INI::loadFileDirectory` walks `<name>\` recursively in case-insensitive sorted order
    /// (`INI.cpp:200-253`), which for a flat key space is exactly this.
    pub fn list_dir(&self, prefix: &str) -> Vec<(String, (usize, usize))> {
        let mut pre = normalise(prefix);
        if !pre.ends_with('\\') {
            pre.push('\\');
        }
        let mut out: Vec<(String, (usize, usize))> = self
            .by_key
            .iter()
            .filter(|(k, _)| k.starts_with(&pre))
            .filter_map(|(k, v)| v.first().map(|i| (k.clone(), *i)))
            .collect();
        out.sort_by(|a, b| a.0.cmp(&b.0));
        out
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// Build a tiny archive in memory so the format is tested without a game installed.
    pub(crate) fn craft(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut table = Vec::new();
        let mut header_len = 16;
        for (n, _) in entries {
            header_len += 8 + n.len() + 1;
        }
        let mut data = Vec::new();
        for (n, body) in entries {
            let off = (header_len + data.len()) as u32;
            table.extend_from_slice(&off.to_be_bytes());
            table.extend_from_slice(&(body.len() as u32).to_be_bytes());
            table.extend_from_slice(n.as_bytes());
            table.push(0);
            data.extend_from_slice(body);
        }
        let mut out = Vec::new();
        out.extend_from_slice(b"BIGF");
        out.extend_from_slice(&((header_len + data.len()) as u32).to_le_bytes());
        out.extend_from_slice(&(entries.len() as u32).to_be_bytes());
        out.extend_from_slice(&(header_len as u32).to_be_bytes());
        out.extend_from_slice(&table);
        out.extend_from_slice(&data);
        out
    }

    #[test]
    fn parses_a_crafted_archive() {
        let bytes = craft(&[
            ("Data\\INI\\CommandSet.ini", b""),
            ("Data\\INI\\CommandButton.ini", b"CommandButton Command_X\nEnd\n"),
        ]);
        let ix = parse_index(&bytes, bytes.len() as u64).expect("crafted archive parses");
        assert_eq!(ix.len(), 2);
        assert_eq!(ix[0].key, "data\\ini\\commandset.ini");
        assert_eq!(ix[0].len, 0, "a zero-byte entry is a real entry");
        assert_eq!(ix[1].len, 28);
    }

    #[test]
    fn refuses_rather_than_panics_on_junk() {
        assert_eq!(parse_index(b"", 0), Err(BigError::TooSmall));
        assert_eq!(parse_index(b"NOPE\0\0\0\0\0\0\0\0\0\0\0\0", 16), Err(BigError::NotBig));
    }

    #[test]
    fn refuses_an_implausible_count_instead_of_looping() {
        let mut b = craft(&[("a", b"x")]);
        b[8..12].copy_from_slice(&0xFFFF_FFFFu32.to_be_bytes());
        assert!(matches!(parse_index(&b, b.len() as u64), Err(BigError::ImplausibleCount(_))));
    }

    #[test]
    fn refuses_a_truncated_table() {
        // A player's archive can be cut short by a failed download. The table then claims entries
        // whose bytes are not there, and the parse must say so rather than slice past the end.
        let full = craft(&[("Data\\INI\\A.ini", b"hello"), ("Data\\INI\\B.ini", b"world")]);
        let cut = &full[..20];
        assert_eq!(parse_index(cut, cut.len() as u64), Err(BigError::TruncatedTable));
    }

    #[test]
    fn refuses_an_entry_pointing_past_the_end() {
        let mut b = craft(&[("x", b"hello")]);
        // Rewrite the first entry's length to something the file does not contain.
        let name_at = 16 + 8;
        assert_eq!(b[name_at], b'x');
        b[20..24].copy_from_slice(&9_999_999u32.to_be_bytes());
        assert!(matches!(
            parse_index(&b, b.len() as u64),
            Err(BigError::EntryOutOfBounds { .. })
        ));
    }

    #[test]
    fn a_name_with_no_terminator_is_unreadable_not_a_long_name() {
        let mut b = craft(&[("x", b"hello")]);
        let name_at = 16 + 8;
        b[name_at + 1] = b'Z'; // clobber the NUL, and nothing after it is zero either
        for i in name_at..b.len() {
            if b[i] == 0 {
                b[i] = b'Z';
            }
        }
        assert_eq!(parse_index(&b, b.len() as u64), Err(BigError::TruncatedTable));
    }

    #[test]
    fn lookup_is_case_and_separator_insensitive() {
        assert_eq!(normalise("Data/INI/CommandSet.ini"), "data\\ini\\commandset.ini");
        assert_eq!(normalise("\\Data\\INI\\CommandSet.ini"), "data\\ini\\commandset.ini");
    }

    #[test]
    fn overlay_is_interleaved_by_name_and_loses_an_exact_tie() {
        // The engine inserts an overlay archive where its OWN basename sorts, and `<= 0` in
        // loadIntoDirectoryTreeAtPriority leaves an exact tie with the copy already mounted.
        let game = vec!["INIZH.big".to_string(), "EnglishZH.big".to_string()];
        let patch = vec!["!!GXCameoHD.big".to_string(), "INIZH.big".to_string()];
        let order = mount_order_of(&game, &patch);
        let names: Vec<&str> = order.iter().map(|(n, _)| n.as_str()).collect();
        assert_eq!(names, vec!["!!GXCameoHD.big", "EnglishZH.big", "INIZH.big", "INIZH.big"]);
        assert!(order[0].1, "the overlay's ! archive sorts to the front");
        assert!(!order[2].1, "on a tie the game folder's copy stays in front");
        assert!(order[3].1);
    }

    #[test]
    fn overlay_is_not_merely_appended() {
        // This is the divergence from the Windows loader: it honours directories in order, so an
        // overlay archive named '!' would lose to a game-folder 'A'. The engine's does not.
        let order = mount_order_of(&["AAA.big".into()], &["!zzz.big".into()]);
        assert_eq!(order[0].0, "!zzz.big");
    }

    // ------------------------------------------------------------------------------------------
    // Against the real archives on this machine. Skips itself when the game is not installed,
    // because "not installed" is an ordinary state and a test that fails on it is noise.
    // ------------------------------------------------------------------------------------------

    fn zh_dir() -> Option<PathBuf> {
        let d = PathBuf::from(std::env::var_os("HOME")?)
            .join("GeneralsX")
            .join("GeneralsZH");
        d.join("INIZH.big").is_file().then_some(d)
    }

    #[test]
    fn reads_the_real_inizh_archive() {
        let Some(dir) = zh_dir() else {
            eprintln!("skipped: no ~/GeneralsX/GeneralsZH/INIZH.big on this machine");
            return;
        };
        let a = Archive::open(&dir.join("INIZH.big")).expect("INIZH.big indexes");
        assert!(a.entries().len() > 100, "got {} entries", a.entries().len());

        let idx = a
            .entries()
            .iter()
            .position(|e| e.key == "data\\ini\\commandbutton.ini")
            .expect("INIZH.big carries CommandButton.ini");
        let body = a.read_entry(idx).expect("its body reads");
        assert_eq!(body.len(), a.entries()[idx].len as usize);
        assert!(
            String::from_utf8_lossy(&body[..64]).contains(';')
                || body.starts_with(b"CommandButton"),
            "the body should look like an INI"
        );
    }
}
