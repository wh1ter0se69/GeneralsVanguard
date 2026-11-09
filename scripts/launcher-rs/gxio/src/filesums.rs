//! Re-verifying the overlay archives an install put on disk.
//!
//! WHY. The signed manifest hashes the payload ZIP, and `update::verify_staged` checks that hash
//! once, when the zip is staged. The `.big` files inside it are never hashed again, and no engine
//! ID hashes archive CONTENT (assetID is the mounted archive NAMES only), so a truncated, corrupt
//! or hand-edited overlay archive is mounted without anyone noticing.
//!
//! HOW. At install, right after the zip's signed hash has verified and the payload is unpacked, the
//! launcher hashes each `.big` it is about to ship and writes `{name, size, hash}` here, in the
//! per-user state folder - NOT in the install folder, where the files it vouches for (and a hand
//! edit) live. At PLAY, [`check`] re-reads that record. The trust root stays the signed zip, at the
//! moment it was verified.
//!
//! BOUND TO ONE FOLDER. The record names the overlay folder it describes. A record for a folder that
//! is no longer the live one (a relocated bundle, a development deploy elsewhere) reads as no
//! record at all, never as a mismatch.
//!
//! NO RECORD IS "CANNOT TELL", NEVER "REFUSE". Installs that predate this, and every platform whose
//! payload is not unpacked by this launcher, have none; they gain one at their next update.
//!
//! LIMITS, stated rather than hidden: the record is bound to a folder PATH, so an app replaced by hand
//! at the same path is compared with the old install's hashes (the refusal says to reinstall); and
//! if the state folder itself cannot be written, a stale record cannot be cleared - nothing in the
//! launcher can work in that state.
//!
//! COST. A cold check hashes every recorded archive once; a warm one stats them. The warm cache is
//! keyed by (name, size, mtime) and holds a name only after the hash matched the record, so it can
//! skip work but never excuse a mismatch. A file edited in place with its size and mtime put back
//! is not caught; that is a deliberate edit by someone who can also edit this folder.

use std::path::{Path, PathBuf};

/// The record, in the launcher's per-user state folder.
pub const RECORD_FILE: &str = "gxfilesums.txt";
/// Names already proven good at a (size, mtime). Pure optimisation; deleting it is always safe.
pub const CACHE_FILE: &str = "gxfilesums-ok.txt";

const HEADER: &str = "gxfilesums 1";
const CACHE_HEADER: &str = "gxfilesums-ok 1";
const MAX_FILE_BYTES: u64 = 4 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileSum {
    pub name: String,
    pub size: u64,
    /// `gxcore::ArtifactHasher` form, the same as a manifest row's.
    pub hash: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    /// The overlay folder these names are relative to.
    pub dir: PathBuf,
    pub sums: Vec<FileSum>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bad {
    pub name: String,
    pub why: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Nothing to compare against. Play goes ahead.
    NoRecord,
    /// Every recorded file that is present matches.
    Ok,
    Bad(Vec<Bad>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Outcome {
    pub verdict: Verdict,
    /// How many files this call actually hashed (the rest were size-checked or cache hits).
    pub hashed: usize,
    /// Recorded archives that are not on disk under either spelling. Not a corruption (the overlay
    /// screen reports missing archives) but never to be logged as "matches".
    pub missing: usize,
}

fn safe_name(n: &str) -> bool {
    !n.is_empty() && !n.contains(['/', '\\', '\n', '\r']) && n != "." && n != ".."
}

fn hash_file(path: &Path) -> std::io::Result<String> {
    let mut h = gxcore::ArtifactHasher::new();
    crate::update::read_chunks(path, &mut |c| h.update(c))?;
    Ok(h.finish())
}

fn mtime_ns(md: &std::fs::Metadata) -> Option<u128> {
    md.modified()
        .ok()?
        .duration_since(std::time::UNIX_EPOCH)
        .ok()
        .map(|d| d.as_nanos())
}

/// Hash every regular `*.big` (or `*.big.off`) directly inside `dir` (a symlink or directory is not one), sorted by
/// name. A name that could not be written on one record line is left out: unrecorded means unchecked.
pub fn collect(dir: &Path) -> Result<Vec<FileSum>, String> {
    let rd = std::fs::read_dir(dir).map_err(|e| format!("could not read {}: {e}", dir.display()))?;
    let mut out = Vec::new();
    for entry in rd {
        let entry = entry.map_err(|e| format!("could not read {}: {e}", dir.display()))?;
        let Some(file) = entry.file_name().to_str().map(str::to_string) else { continue };
        // An archive shipped switched OFF is `x.big.off` and is recorded under `x.big`, the name
        // it carries once the player switches it on (game::set_optional renames between the two).
        let off = file.to_ascii_lowercase().ends_with(".big.off");
        let name = if off { file[..file.len() - 4].to_string() } else { file };
        if !safe_name(&name) || !name.to_ascii_lowercase().ends_with(".big") {
            continue;
        }
        if off && dir.join(&name).exists() {
            continue; // both spellings present: the mounted one is the one recorded
        }
        let md = std::fs::symlink_metadata(entry.path())
            .map_err(|e| format!("could not read {}: {e}", entry.path().display()))?;
        if !md.is_file() {
            continue;
        }
        let hash = hash_file(&entry.path())
            .map_err(|e| format!("could not hash {}: {e}", entry.path().display()))?;
        out.push(FileSum { name, size: md.len(), hash });
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(out)
}

pub fn render(dir: &Path, sums: &[FileSum]) -> Option<String> {
    let d = dir.to_str().filter(|d| !d.contains('\n'))?;
    let mut s = format!("{HEADER}\ndir {d}\n");
    for f in sums.iter().filter(|f| safe_name(&f.name)) {
        s.push_str(&format!("{} {} {}\n", f.size, f.hash, f.name));
    }
    Some(s)
}

/// `None` for anything this module did not write.
pub fn parse(text: &str) -> Option<Record> {
    let mut lines = text.lines();
    if lines.next()? != HEADER {
        return None;
    }
    let dir = PathBuf::from(lines.next()?.strip_prefix("dir ")?);
    let mut sums = Vec::new();
    for l in lines {
        let mut p = l.splitn(3, ' ');
        let (Some(size), Some(hash), Some(name)) = (p.next(), p.next(), p.next()) else { continue };
        let Ok(size) = size.parse::<u64>() else { continue };
        if safe_name(name) && !hash.is_empty() {
            sums.push(FileSum { name: name.to_string(), size, hash: hash.to_string() });
        }
    }
    Some(Record { dir, sums })
}

fn read_bounded(path: &Path) -> Option<String> {
    let md = std::fs::symlink_metadata(path).ok()?;
    if !md.is_file() || md.len() > MAX_FILE_BYTES {
        return None;
    }
    std::fs::read_to_string(path).ok()
}

/// Write the record for `dir`. Call it only AFTER the payload's signed hash has verified.
pub fn write_record(record: &Path, dir: &Path, sums: &[FileSum]) -> Result<(), String> {
    let text = render(dir, sums).ok_or_else(|| "the overlay folder has an unwritable name".to_string())?;
    crate::release::write_atomic(record, text).map_err(|e| format!("could not write {}: {e}", record.display()))?;
    // A new record makes every earlier "already proven" entry meaningless.
    let _ = std::fs::remove_file(record.with_file_name(CACHE_FILE));
    Ok(())
}

/// The engine bundle was moved: point the record at the new overlay folder, or it would read as a
/// record for some other folder and stop checking. Nothing happens unless the record names `old`.
pub fn rebind(record: &Path, old: &Path, new: &Path) {
    let Some(rec) = read_bounded(record).and_then(|t| parse(&t)) else { return };
    if rec.dir == old {
        let _ = write_record(record, new, &rec.sums);
    }
}

/// Make a record that can no longer be trusted stop applying. Deleting is the answer; if the delete
/// is refused, an empty record for no folder is written over it, which checks nothing. Either way a
/// stale record can never refuse a legitimate file.
pub fn clear_record(record: &Path) {
    let _ = std::fs::remove_file(record);
    if record.exists() {
        let _ = write_record(record, Path::new("/"), &[]);
    }
}

/// The record's text, to put back if the install that is about to clear it fails.
pub fn take_record(record: &Path) -> Option<String> {
    read_bounded(record)
}

/// Put back what [`take_record`] returned (best effort).
pub fn restore_record(record: &Path, text: &str) {
    let _ = crate::release::write_atomic(record, text);
}

fn read_cache(path: &Path) -> Vec<(String, u64, u128, String)> {
    let Some(text) = read_bounded(path) else { return Vec::new() };
    let mut lines = text.lines();
    if lines.next() != Some(CACHE_HEADER) {
        return Vec::new();
    }
    lines
        .filter_map(|l| {
            let mut p = l.splitn(4, ' ');
            let size = p.next()?.parse().ok()?;
            let mt = p.next()?.parse().ok()?;
            let hash = p.next()?.to_string();
            let name = p.next()?.to_string();
            Some((name, size, mt, hash))
        })
        .collect()
}

/// Compare `dir` against the record at `record_path`.
///
/// * a file the record does not list is the player's and is ignored;
/// * a listed file that is absent under both spellings (`x.big`, `x.big.off`) is ignored - the overlay
///   screen reports missing archives;
/// * a listed file whose size differs is refused without hashing it;
/// * otherwise it is hashed unless `cache_path` holds this exact (size, mtime) as already good.
pub fn check(dir: &Path, record_path: &Path, cache_path: &Path) -> Outcome {
    let no = Outcome { verdict: Verdict::NoRecord, hashed: 0, missing: 0 };
    let Some(rec) = read_bounded(record_path).and_then(|t| parse(&t)) else { return no };
    if rec.dir != dir {
        return no;
    }
    let cache = read_cache(cache_path);
    let mut bad = Vec::new();
    let mut good: Vec<String> = Vec::new();
    let mut hashed = 0usize;
    let mut missing = 0usize;
    for f in &rec.sums {
        let mut p = dir.join(&f.name);
        let mut found = std::fs::symlink_metadata(&p);
        if found.is_err() {
            // Switched off since the install: the same bytes under the other spelling.
            p = dir.join(format!("{}.off", f.name));
            found = std::fs::symlink_metadata(&p);
        }
        let md = match found {
            Ok(md) => md,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                missing += 1;
                continue;
            }
            // Present but unreadable (permissions, I/O) is not "absent": it cannot be vouched for.
            Err(e) => {
                bad.push(Bad { name: f.name.clone(), why: format!("could not be examined ({e})") });
                continue;
            }
        };
        if !md.is_file() {
            bad.push(Bad { name: f.name.clone(), why: "is no longer a regular file".into() });
            continue;
        }
        if md.len() != f.size {
            bad.push(Bad {
                name: f.name.clone(),
                why: format!("is {} bytes, the install wrote {} (cut short or edited)", md.len(), f.size),
            });
            continue;
        }
        let mt = mtime_ns(&md);
        let cached = mt.is_some_and(|mt| {
            cache.iter().any(|(n, s, m, h)| *n == f.name && *s == f.size && *m == mt && *h == f.hash)
        });
        if !cached {
            hashed += 1;
            match hash_file(&p) {
                Ok(h) if h == f.hash => {}
                Ok(_) => {
                    bad.push(Bad { name: f.name.clone(), why: "does not match what the install wrote".into() });
                    continue;
                }
                Err(e) => {
                    bad.push(Bad { name: f.name.clone(), why: format!("could not be read ({e})") });
                    continue;
                }
            }
        }
        if let Some(mt) = mt {
            good.push(format!("{} {} {} {}", f.size, mt, f.hash, f.name));
        }
    }
    // Rewritten from this run's matches only, so an entry for a file that has since changed or gone
    // does not linger. Best effort: losing the cache costs a re-hash, nothing else.
    let mut text = format!("{CACHE_HEADER}\n");
    for g in &good {
        text.push_str(g);
        text.push('\n');
    }
    let _ = crate::release::write_atomic(cache_path, text);
    Outcome { verdict: if bad.is_empty() { Verdict::Ok } else { Verdict::Bad(bad) }, hashed, missing }
}

pub fn record_path() -> Option<PathBuf> {
    crate::state::state_dir().map(|d| d.join(RECORD_FILE))
}

fn cache_path() -> Option<PathBuf> {
    crate::state::state_dir().map(|d| d.join(CACHE_FILE))
}

/// PLAY's call: check the live overlay folder against the install's own record.
pub fn check_here(dir: &Path) -> Outcome {
    match (record_path(), cache_path()) {
        (Some(r), Some(c)) => check(dir, &r, &c),
        _ => Outcome { verdict: Verdict::NoRecord, hashed: 0, missing: 0 },
    }
}

/// The phrase the launcher page looks for to offer its Repair button. Rust owns the wording; the
/// page (design/launcher/screens/MainWeb.html) carries the same literal, pinned by a test on each side.
pub const REFUSAL_MARK: &str = "an installed game archive changed";

/// The sentence PLAY refuses with.
pub fn refusal(bad: &[Bad]) -> String {
    let list = bad
        .iter()
        .take(3)
        .map(|b| format!("{} {}", b.name, b.why))
        .collect::<Vec<_>>()
        .join("; ");
    let more = bad.len().saturating_sub(3);
    let tail = if more > 0 { format!(" (and {more} more)") } else { String::new() };
    format!(
        "GeneralsX did not start: {REFUSAL_MARK} after it was installed - {list}{tail}. \
         Press Repair to download it again."
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    struct T(PathBuf);
    impl T {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!("gxsums-{tag}-{}", crate::update::unique_suffix()));
            std::fs::create_dir_all(p.join("ov")).unwrap();
            T(p)
        }
        fn ov(&self) -> PathBuf {
            self.0.join("ov")
        }
        fn rec(&self) -> PathBuf {
            self.0.join("state").join(RECORD_FILE)
        }
        fn cache(&self) -> PathBuf {
            self.0.join("state").join(CACHE_FILE)
        }
        /// Install two archives and write the record the way an install does.
        fn installed(&self) {
            std::fs::write(self.ov().join("!!GXModelsHD.big"), vec![7u8; 5000]).unwrap();
            std::fs::write(self.ov().join("a.big"), b"small archive").unwrap();
            let sums = collect(&self.ov()).unwrap();
            write_record(&self.rec(), &self.ov(), &sums).unwrap();
        }
        fn check(&self) -> Outcome {
            check(&self.ov(), &self.rec(), &self.cache())
        }
    }
    impl Drop for T {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn a_matching_install_passes() {
        let t = T::new("ok");
        t.installed();
        assert_eq!(t.check().verdict, Verdict::Ok);
    }

    #[test]
    fn a_flipped_byte_is_refused() {
        let t = T::new("flip");
        t.installed();
        let p = t.ov().join("!!GXModelsHD.big");
        let mut b = std::fs::read(&p).unwrap();
        b[2500] ^= 1;
        std::fs::write(&p, b).unwrap(); // same size
        match t.check().verdict {
            Verdict::Bad(v) => assert_eq!(v[0].name, "!!GXModelsHD.big"),
            v => panic!("{v:?}"),
        }
    }

    #[test]
    fn a_truncated_file_is_refused_without_hashing_it() {
        let t = T::new("trunc");
        t.installed();
        std::fs::write(t.ov().join("!!GXModelsHD.big"), vec![7u8; 100]).unwrap();
        let o = t.check();
        assert!(matches!(o.verdict, Verdict::Bad(_)), "{o:?}");
        assert_eq!(o.hashed, 1, "only the other, intact archive is hashed; the short one is decided from its size");
    }

    #[test]
    fn an_unchanged_file_is_hashed_once_then_served_from_the_cache() {
        let t = T::new("cache");
        t.installed();
        let first = t.check();
        assert_eq!((first.verdict, first.hashed), (Verdict::Ok, 2), "cold: both hashed");
        let second = t.check();
        assert_eq!((second.verdict, second.hashed), (Verdict::Ok, 0), "warm: nothing hashed");
    }

    #[test]
    fn a_changed_mtime_or_size_rehashes() {
        let t = T::new("rehash");
        t.installed();
        t.check();
        let p = t.ov().join("a.big");
        let f = std::fs::OpenOptions::new().write(true).open(&p).unwrap();
        f.set_modified(std::time::SystemTime::now() + std::time::Duration::from_secs(5)).unwrap();
        drop(f);
        let o = t.check();
        assert_eq!((o.verdict, o.hashed), (Verdict::Ok, 1), "only the touched file is re-hashed");
    }

    #[test]
    fn a_cache_hit_never_excuses_a_changed_file() {
        let t = T::new("excuse");
        t.installed();
        t.check(); // fills the cache
        let p = t.ov().join("a.big");
        std::fs::write(&p, b"smallXarchive").unwrap(); // same size, new mtime
        assert!(matches!(t.check().verdict, Verdict::Bad(_)));
    }

    #[test]
    fn a_file_outside_the_record_is_ignored() {
        let t = T::new("own");
        t.installed();
        std::fs::write(t.ov().join("mine.big"), b"the player's own").unwrap();
        std::fs::write(t.ov().join("notes.txt"), b"x").unwrap();
        let o = t.check();
        assert_eq!((o.verdict, o.hashed), (Verdict::Ok, 2));
        assert!(t.ov().join("mine.big").exists(), "and it is never deleted");
    }

    #[test]
    fn no_record_does_not_block() {
        let t = T::new("none");
        std::fs::write(t.ov().join("a.big"), b"x").unwrap();
        assert_eq!(t.check().verdict, Verdict::NoRecord);
    }

    #[test]
    fn a_record_for_another_folder_does_not_block() {
        let t = T::new("moved");
        t.installed();
        let other = t.0.join("elsewhere");
        std::fs::create_dir_all(&other).unwrap();
        std::fs::write(other.join("a.big"), b"different").unwrap();
        assert_eq!(check(&other, &t.rec(), &t.cache()).verdict, Verdict::NoRecord);
    }

    #[test]
    fn a_switched_off_or_absent_archive_is_not_a_corruption() {
        let t = T::new("off");
        t.installed();
        std::fs::rename(t.ov().join("a.big"), t.ov().join("a.big.off")).unwrap();
        assert_eq!(t.check().verdict, Verdict::Ok);
    }

    #[test]
    fn an_archive_shipped_switched_off_is_recorded_and_checked_once_switched_on() {
        let t = T::new("shipped-off");
        std::fs::write(t.ov().join("opt.big.off"), vec![3u8; 4000]).unwrap();
        let sums = collect(&t.ov()).unwrap();
        assert_eq!(sums[0].name, "opt.big");
        write_record(&t.rec(), &t.ov(), &sums).unwrap();
        assert_eq!(t.check().verdict, Verdict::Ok, "checked under its .off spelling");
        std::fs::rename(t.ov().join("opt.big.off"), t.ov().join("opt.big")).unwrap(); // the player enables it
        assert_eq!(t.check().verdict, Verdict::Ok);
        let mut b = std::fs::read(t.ov().join("opt.big")).unwrap();
        b[10] ^= 1;
        std::fs::write(t.ov().join("opt.big"), b).unwrap();
        assert!(matches!(t.check().verdict, Verdict::Bad(_)));
    }

    #[test]
    fn a_moved_bundle_keeps_being_checked_once_the_record_is_rebound() {
        let t = T::new("rebind");
        t.installed();
        let moved = t.0.join("moved-ov");
        std::fs::rename(t.ov(), &moved).unwrap();
        assert_eq!(check(&moved, &t.rec(), &t.cache()).verdict, Verdict::NoRecord, "unbound: the move hides it");
        rebind(&t.rec(), &t.ov(), &moved);
        assert_eq!(check(&moved, &t.rec(), &t.cache()).verdict, Verdict::Ok);
        let mut b = std::fs::read(moved.join("a.big")).unwrap();
        b[0] ^= 1;
        std::fs::write(moved.join("a.big"), b).unwrap();
        assert!(matches!(check(&moved, &t.rec(), &t.cache()).verdict, Verdict::Bad(_)));
    }

    #[test]
    fn a_deleted_archive_is_counted_missing_not_reported_as_matching() {
        let t = T::new("gone");
        t.installed();
        std::fs::remove_file(t.ov().join("a.big")).unwrap();
        let o = t.check();
        assert_eq!((o.verdict, o.missing, o.hashed), (Verdict::Ok, 1, 1));
    }

    #[test]
    fn a_cleared_record_stops_applying_and_a_taken_one_can_be_put_back() {
        let t = T::new("clear");
        t.installed();
        let text = take_record(&t.rec()).unwrap();
        clear_record(&t.rec());
        assert_eq!(t.check().verdict, Verdict::NoRecord);
        restore_record(&t.rec(), &text);
        assert_eq!(t.check().verdict, Verdict::Ok, "the old install is protected again");
        std::fs::write(t.ov().join("a.big"), b"smallXarchive").unwrap();
        assert!(matches!(t.check().verdict, Verdict::Bad(_)));
    }

    #[test]
    fn the_refusal_carries_the_phrase_the_page_looks_for_and_the_file() {
        let r = refusal(&[Bad { name: "x.big".into(), why: "is cut short".into() }]);
        assert!(r.contains(REFUSAL_MARK) && r.contains("x.big") && r.contains("Repair"));
    }

    #[test]
    fn a_garbled_record_reads_as_no_record() {
        let t = T::new("junk");
        std::fs::create_dir_all(t.0.join("state")).unwrap();
        std::fs::write(t.rec(), "not a record\n").unwrap();
        assert_eq!(t.check().verdict, Verdict::NoRecord);
    }

    #[test]
    fn collect_takes_only_regular_big_files() {
        let t = T::new("collect");
        std::fs::write(t.ov().join("x.big"), b"1").unwrap();
        std::fs::write(t.ov().join("x.big.off"), b"2").unwrap();
        std::fs::write(t.ov().join("y.ini"), b"3").unwrap();
        std::fs::create_dir_all(t.ov().join("d.big")).unwrap();
        let names: Vec<_> = collect(&t.ov()).unwrap().into_iter().map(|s| s.name).collect();
        assert_eq!(names, vec!["x.big"], "x.big.off is the same archive switched off, not a second one");
    }
}
