//! The two answers the first-run setup panel can record, and the file they live in.
//!
//! # Why a store at all, when the panel asks four questions
//!
//! Two of the panel's questions already have somewhere to go. "Where is your copy of Zero Hour"
//! is [`crate::game::set_chosen_data_dir`], which has existed since the setup page did. "Set it
//! all up for me" versus "Let me choose" is the state of a disclosure, not a setting - it says
//! which controls are on screen and nothing else, so persisting it would be persisting a scroll
//! position.
//!
//! The other two have nowhere. Where the engine lands is [`crate::engine::engine_install_dir`],
//! which was a constant expression; whether the community-patch overlay is mounted is a property
//! of the folder [`crate::engine::apply_engine_payload`] stages, which nothing asked about. Both
//! are consulted by code that runs long after the panel is gone - an automatic update on a later
//! launch installs to the same place and must leave the overlay the way the player asked for it -
//! so they have to be facts on disk rather than variables in a webview.
//!
//! # THE DEFAULT IS THE AUTOMATIC PATH'S OWN ANSWER, AND THAT IS LOAD-BEARING
//!
//! [`SetupChoices::default`] is `Applications` + `Install`, which is exactly what
//! "Set it all up for me" does. So the player who never opens the door writes NOTHING - an absent
//! file, an unreadable one, a corrupt one and a deliberate "set it all up for me" are all the same
//! state, and they are the safe one. Only a DEVIATION is ever written.
//!
//! That is also what makes the panel's promise checkable rather than merely stated: opening
//! "Let me choose" and changing nothing produces an identical result because the manual path's
//! starting values ARE these defaults, not because two code paths were written to agree.
//!
//! # The file
//!
//! `gxsetup.txt` in [`crate::state::state_dir`], beside `gxdatadir.txt`
//! ([`crate::game::CHOSEN_DATA_FILE`]) and `gxenginestate.txt`
//! ([`crate::engine::ENGINE_STATE_FILE`]). Same shape as the latter - `key value` lines, one per
//! fact, unrecognised lines dropped - so a key added later by a newer build is ignored by an older
//! one instead of making the whole file unreadable.
//!
//! ```text
//! enginedir besidedata
//! patch skip
//! ```
//!
//! A KEY WHOSE VALUE IS NOT UNDERSTOOD FALLS BACK TO THE DEFAULT rather than to the last thing
//! parsed. A newer build writing `enginedir somewhere-else` must not make an older build install
//! the engine into a folder it has never heard of.

use std::path::{Path, PathBuf};

/// Where a freshly installed engine bundle goes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum EngineLocation {
    /// `~/Applications/GeneralsXZH.app` - the automatic path's answer, and the only one this
    /// launcher used before the setup panel existed. See [`crate::engine::engine_install_dir`]
    /// for the whole argument about why not `/Applications`.
    #[default]
    Applications,
    /// `~/GeneralsX/GeneralsXZH.app` - a sibling of `~/GeneralsX/GeneralsZH`, the folder
    /// `deploy-macos-zh.sh` puts the retail data in. NOT inside it: the data folder is the one
    /// thing this launcher never writes into, and
    /// `engine_install_dir_is_the_per_user_applications_bundle_and_nothing_else` still holds that
    /// line for both answers.
    BesideData,
}

impl EngineLocation {
    /// The wire token. Stable: it is written into a file an older build may read back.
    pub fn as_str(self) -> &'static str {
        match self {
            EngineLocation::Applications => "applications",
            EngineLocation::BesideData => "besidedata",
        }
    }

    /// `None` for anything this build does not understand - see this module's header for why that
    /// is not the same as "the last value that parsed".
    pub fn from_token(s: &str) -> Option<EngineLocation> {
        match s.trim().to_ascii_lowercase().as_str() {
            "applications" => Some(EngineLocation::Applications),
            "besidedata" => Some(EngineLocation::BesideData),
            _ => None,
        }
    }
}

/// Whether the community-patch overlay the engine payload ships is left mounted.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PatchChoice {
    /// Leave every archive the payload staged where the engine will load it. The automatic path.
    #[default]
    Install,
    /// Switch the whole managed set off, by rename, inside the folder the install just staged.
    ///
    /// THE COST IS REAL AND THE PANEL SAYS SO: an install without the required archives computes a
    /// different `dataID` from everyone else's and is refused by every lobby that has them. See
    /// [`crate::game::REQUIRED_REFUSAL`], which is the same fact worded for the settings screen -
    /// where the answer is no, because there it is one mis-click rather than a first-run question
    /// with the consequence written under it.
    Skip,
}

impl PatchChoice {
    pub fn as_str(self) -> &'static str {
        match self {
            PatchChoice::Install => "install",
            PatchChoice::Skip => "skip",
        }
    }

    pub fn from_token(s: &str) -> Option<PatchChoice> {
        match s.trim().to_ascii_lowercase().as_str() {
            "install" => Some(PatchChoice::Install),
            "skip" => Some(PatchChoice::Skip),
            _ => None,
        }
    }

    /// True when the overlay must end up mounted.
    pub fn wants_patch(self) -> bool {
        matches!(self, PatchChoice::Install)
    }
}

/// Everything the first-run panel records. See this module's header for what it deliberately does
/// not record.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct SetupChoices {
    pub engine_location: EngineLocation,
    pub patch: PatchChoice,
}

impl SetupChoices {
    /// Nothing deviates from the automatic path, so there is nothing worth keeping on disk.
    ///
    /// [`write_setup_choices_to`] uses this to REMOVE the file rather than write the defaults into
    /// it, which is what keeps "never opened the door" and "opened it and changed nothing"
    /// indistinguishable on disk as well as on screen.
    pub fn is_default(&self) -> bool {
        *self == SetupChoices::default()
    }
}

/// The file the answers live in. See this module's header for the format.
pub const SETUP_CHOICES_FILE: &str = "gxsetup.txt";

/// Where that file lives. `None` only when the platform has no home directory.
pub fn setup_choices_path() -> Option<PathBuf> {
    crate::state::state_dir().map(|d| d.join(SETUP_CHOICES_FILE))
}

/// Files whose last [`read_setup_choices_for_page_at`] found them there and unreadable (see there).
static UNREAD_AT_PAGE: std::sync::Mutex<std::collections::BTreeSet<PathBuf>> =
    std::sync::Mutex::new(std::collections::BTreeSet::new());

fn unread_at_page() -> std::sync::MutexGuard<'static, std::collections::BTreeSet<PathBuf>> {
    UNREAD_AT_PAGE.lock().unwrap_or_else(|p| p.into_inner())
}

/// What the setup panel shows: [`read_setup_choices`], plus the one thing the panel needs that the engine flows do not.
/// A file that is THERE and cannot be read exactly (an error, or over the size this launcher writes) is marked, and every
/// [`write_setup_choices_to`] of it is refused for the rest of this launcher's run.
///
/// GeneralsX @bugfix Claude 22/10/2026 (Codex fwc-r2/r4/r5 P2) The panel sends what it shows, and it seeds what it shows
/// ONCE per page load from the first state it gets (SetupMac.html `choicesSeeded`); later states - setup_state again,
/// setup_state_checked's refresh - never re-seed it. So a panel that opened on a failed read holds the defaults for its
/// whole life, and a later good read must NOT lift the mark: it used to, and SET UP & PLAY then deleted the recorded
/// answers. The mark lasts the process; the write's refusal says to restart the launcher. Only THIS read sets it - the
/// engine flows' read_setup_choices() never does. A first run has no file, so it is never marked.
pub fn read_setup_choices_for_page() -> SetupChoices {
    match setup_choices_path() {
        Some(p) => read_setup_choices_for_page_at(&p),
        None => SetupChoices::default(),
    }
}

/// [`read_setup_choices_for_page`] against a named file. ONE read: what it shows is what it judged (Codex fwc-r5 P2 -
/// a probe read that succeeded and a second read that failed showed the defaults with no mark).
pub fn read_setup_choices_for_page_at(path: &Path) -> SetupChoices {
    let got = match std::fs::metadata(path) {
        Ok(md) if md.len() > MAX_SETUP_CHOICES_BYTES => None,
        _ => match std::fs::read(path) {
            Ok(bytes) => Some(parse_setup_choices(&String::from_utf8_lossy(&bytes))),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound && std::fs::symlink_metadata(path).is_err() => {
                Some(SetupChoices::default())
            }
            Err(_) => None,
        },
    };
    match got {
        Some(c) => c,
        None => {
            unread_at_page().insert(path.to_path_buf());
            SetupChoices::default()
        }
    }
}

/// The recorded answers, or the automatic path's own when nothing is recorded.
///
/// NEVER FAILS. Every failure mode - no home directory, no file, an unreadable file, a file full
/// of tokens this build does not know - answers with the default, which is the automatic path. A
/// launcher that refused to start because a preferences file was corrupt would be trading a
/// recoverable state for an unrecoverable one.
pub fn read_setup_choices() -> SetupChoices {
    match setup_choices_path() {
        Some(p) => read_setup_choices_from(&p),
        None => SetupChoices::default(),
    }
}

/// [`read_setup_choices`] against a named file.
///
/// Split out for the same reason [`crate::game::read_chosen_data_dir`] is: `state_dir()` is
/// resolved from `$HOME`, which is process-wide, and a test that repointed it would repoint it for
/// every other test running beside it.
pub fn read_setup_choices_from(path: &Path) -> SetupChoices {
    // A file this size is not one this launcher wrote. Read nothing rather than parse it.
    if let Ok(md) = std::fs::metadata(path) {
        if md.len() > MAX_SETUP_CHOICES_BYTES {
            return SetupChoices::default();
        }
    }
    // GeneralsX @bugfix Claude 22/10/2026 Decoded lossily: `read_to_string` read a file with one byte that is not UTF-8
    // (a Latin-1 comment) as nothing recorded, and SET UP & PLAY then sent those defaults and deleted the answers
    // (Codex fwc-r1 P2). The parser only matches ASCII tokens, so a U+FFFD can change nothing it reads.
    let Ok(bytes) = std::fs::read(path) else {
        return SetupChoices::default();
    };
    parse_setup_choices(&String::from_utf8_lossy(&bytes))
}

/// A corrupt or absurdly large file reads as nothing recorded rather than being parsed line by
/// line - the same ceiling and the same reasoning as
/// [`crate::engine::ENGINE_STATE_FILE`]'s own reader.
const MAX_SETUP_CHOICES_BYTES: u64 = 4096;

/// The pure parser. Unrecognised keys and unrecognised values both fall through to the default for
/// that one fact, and never to the default for the whole file: a build that understands
/// `enginedir` and not some future key must still honour the `enginedir` line it can read.
pub fn parse_setup_choices(text: &str) -> SetupChoices {
    let mut out = SetupChoices::default();
    for line in text.lines() {
        let line = line.trim();
        let Some((key, value)) = line.split_once(char::is_whitespace) else {
            continue;
        };
        match key.trim().to_ascii_lowercase().as_str() {
            "enginedir" => {
                if let Some(v) = EngineLocation::from_token(value) {
                    out.engine_location = v;
                }
            }
            "patch" => {
                if let Some(v) = PatchChoice::from_token(value) {
                    out.patch = v;
                }
            }
            _ => {}
        }
    }
    out
}

/// The text form of a set of answers, for [`write_setup_choices_to`] and for the test that pins
/// the round trip.
pub fn render_setup_choices(c: &SetupChoices) -> String {
    format!(
        "enginedir {}\npatch {}\n",
        c.engine_location.as_str(),
        c.patch.as_str()
    )
}

/// Record the answers.
pub fn write_setup_choices(c: &SetupChoices) -> Result<(), String> {
    let path = setup_choices_path()
        .ok_or_else(|| "There is no per-user folder to remember your answers in.".to_string())?;
    write_setup_choices_to(&path, c)
}

/// Whether `path` may be written at all - every check [`write_setup_choices_to`] makes before it touches anything, for a
/// caller whose own earlier steps must not run either when the write would be refused (Codex fwc-r6 P2:
/// `engine::apply_setup_choices_in` moved the engine bundle FIRST, then reached the refusal).
pub fn writable(path: &Path) -> Result<(), String> {
    if unread_at_page().contains(path) {
        return Err(format!(
            "{} could not be read when the setup panel opened, so it is left as it is. Quit the launcher and open it again.",
            path.display()
        ));
    }
    // GeneralsX @bugfix Claude 22/10/2026 A file that is there and could not be read is never replaced or deleted: what
    // the panel showed came from read_setup_choices_from, which reads such a file - and one over the size this
    // launcher writes - as nothing recorded (file-wipe class, Codex fwc-r1 P2). A dangling symlink counts as there.
    match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(format!("{} could not be checked ({e}), so it is left as it is.", path.display())),
        Ok(_) if std::fs::metadata(path).is_ok_and(|md| md.len() > MAX_SETUP_CHOICES_BYTES) => {
            // metadata, not the link's own: the reader's size rule follows a link too (Codex fwc-r2 P2).
            return Err(format!("{} is not a file this launcher wrote, so it is left as it is.", path.display()))
        }
        Ok(_) => {
            if let Err(e) = std::fs::read(path) {
                return Err(format!("{} could not be read ({e}), so it is left as it is.", path.display()));
            }
        }
    }
    Ok(())
}

/// [`write_setup_choices`] against a named file. Same split, same reason, as the reader.
///
/// THE DEFAULT IS WRITTEN AS AN ABSENCE. See [`SetupChoices::is_default`] - a player who takes the
/// automatic path, or who opens the door and puts everything back, leaves no file behind, so the
/// state on disk is the same state a machine that has never seen this panel is in.
pub fn write_setup_choices_to(path: &Path, c: &SetupChoices) -> Result<(), String> {
    writable(path)?;
    if c.is_default() {
        return match std::fs::remove_file(path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(format!("Could not clear {}: {e}", path.display())),
        };
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Could not prepare {}: {e}", parent.display()))?;
    }
    // Temp-then-rename, exactly like `game::write_chosen_data_dir`: a half-written line here is an
    // install location nobody chose.
    let tmp = path.with_extension("gxtmp");
    std::fs::write(&tmp, render_setup_choices(c))
        .map_err(|e| format!("Could not write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("Could not save your setup answers: {e}")
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Tmp(PathBuf);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let p = std::env::temp_dir().join(format!(
                "gxsetupchoices-{tag}-{}",
                crate::update::unique_suffix()
            ));
            std::fs::create_dir_all(&p).expect("scratch dir");
            Tmp(p)
        }
        fn file(&self) -> PathBuf {
            self.0.join(SETUP_CHOICES_FILE)
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    // GeneralsX @test Claude 22/10/2026 An existing answers file this cannot read is never replaced or deleted
    // (Codex fwc-r1 P2): a stray byte read the whole file as nothing recorded, and taking the defaults deleted it.
    #[test]
    fn a_stray_byte_still_reads_the_answers() {
        let t = Tmp::new("lossy");
        std::fs::write(t.file(), b"# caf\xe9\nenginedir besidedata\npatch skip\n").unwrap();
        let c = read_setup_choices_from(&t.file());
        assert!(!c.is_default(), "the answers were not read: {c:?}");
        assert_eq!(c.patch, PatchChoice::Skip);
    }

    #[cfg(unix)]
    #[test]
    fn a_file_that_cannot_be_read_is_never_replaced_or_deleted() {
        use std::os::unix::fs::PermissionsExt;
        let t = Tmp::new("noread");
        let body = b"enginedir besidedata\npatch skip\n";
        std::fs::write(t.file(), body).unwrap();
        std::fs::set_permissions(t.file(), std::fs::Permissions::from_mode(0o200)).unwrap();
        let shown = read_setup_choices_from(&t.file());
        let clear = write_setup_choices_to(&t.file(), &shown);
        let other = write_setup_choices_to(&t.file(), &parse_setup_choices("patch skip\n"));
        let mode = std::fs::metadata(t.file()).map(|m| m.permissions().mode() & 0o777);
        std::fs::set_permissions(t.file(), std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(clear.is_err(), "taking the defaults deleted a file that cannot be read");
        assert!(other.is_err(), "a save replaced a file that cannot be read");
        assert_eq!(mode.ok(), Some(0o200), "the file was replaced or deleted");
        assert_eq!(std::fs::read(t.file()).unwrap(), body);
    }

    /// Codex fwc-r4 P2: the panel opened while the file could not be read (it shows the defaults), the file became
    /// readable, the player pressed SET UP & PLAY. The failed PAGE read is what must block the write.
    #[cfg(unix)]
    #[test]
    fn a_panel_that_opened_on_an_unreadable_file_writes_nothing_for_the_rest_of_the_run() {
        use std::os::unix::fs::PermissionsExt;
        let t = Tmp::new("transient");
        let body = b"enginedir besidedata\npatch skip\n";
        std::fs::write(t.file(), body).unwrap();
        std::fs::set_permissions(t.file(), std::fs::Permissions::from_mode(0o200)).unwrap();
        let shown = read_setup_choices_for_page_at(&t.file());
        std::fs::set_permissions(t.file(), std::fs::Permissions::from_mode(0o600)).unwrap();
        assert!(shown.is_default(), "an unreadable file shows the defaults");
        assert!(write_setup_choices_to(&t.file(), &shown).is_err(), "the panel's defaults deleted the answers");
        assert_eq!(std::fs::read(t.file()).unwrap(), body, "the answers must be exactly as they were");
        // Codex fwc-r5 P2: a later page read that succeeds (setup_state again, or setup_state_checked's refresh) must NOT
        // lift the mark - the panel never re-seeds, so it still holds the defaults.
        let later = read_setup_choices_for_page_at(&t.file());
        assert_eq!(later.patch, PatchChoice::Skip, "a later read sees the real answers");
        assert!(write_setup_choices_to(&t.file(), &shown).is_err(), "a later good read lifted the mark");
        assert_eq!(std::fs::read(t.file()).unwrap(), body);
        // The control: a panel whose read succeeded writes as before (taking the defaults clears the file).
        let t2 = Tmp::new("transient-ok");
        std::fs::write(t2.file(), body).unwrap();
        assert_eq!(read_setup_choices_for_page_at(&t2.file()).patch, PatchChoice::Skip);
        write_setup_choices_to(&t2.file(), &SetupChoices::default()).expect("a write after a good page read");
        assert!(!t2.file().exists(), "taking the defaults after a good read clears the file as before");
    }

    /// The whole safety argument in one assertion: nothing recorded IS the automatic path.
    #[test]
    fn nothing_recorded_is_the_automatic_paths_own_answer() {
        let t = Tmp::new("absent");
        let c = read_setup_choices_from(&t.file());
        assert_eq!(c.engine_location, EngineLocation::Applications);
        assert_eq!(c.patch, PatchChoice::Install);
        assert!(c.is_default());
    }

    #[test]
    fn a_deviation_survives_a_round_trip() {
        let t = Tmp::new("roundtrip");
        let want = SetupChoices {
            engine_location: EngineLocation::BesideData,
            patch: PatchChoice::Skip,
        };
        write_setup_choices_to(&t.file(), &want).expect("written");
        assert_eq!(read_setup_choices_from(&t.file()), want);
    }

    /// Putting everything back is indistinguishable from never having touched it - on disk, not
    /// merely on screen. See [`SetupChoices::is_default`].
    #[test]
    fn putting_every_answer_back_removes_the_file_rather_than_writing_the_defaults() {
        let t = Tmp::new("back");
        write_setup_choices_to(
            &t.file(),
            &SetupChoices {
                engine_location: EngineLocation::BesideData,
                patch: PatchChoice::Skip,
            },
        )
        .expect("written");
        assert!(t.file().is_file());

        write_setup_choices_to(&t.file(), &SetupChoices::default()).expect("cleared");
        assert!(
            !t.file().exists(),
            "the automatic path must leave no file behind, or 'never opened the door' and \
             'opened it and changed nothing' stop being the same state"
        );
        assert_eq!(read_setup_choices_from(&t.file()), SetupChoices::default());
    }

    #[test]
    fn a_value_this_build_does_not_understand_falls_back_to_the_default_for_that_key_alone() {
        let c = parse_setup_choices("enginedir somewhere-else\npatch skip\n");
        assert_eq!(
            c.engine_location,
            EngineLocation::Applications,
            "an install location this build has never heard of must not be honoured"
        );
        assert_eq!(c.patch, PatchChoice::Skip, "and the key it CAN read still counts");
    }

    #[test]
    fn an_unknown_key_is_dropped_the_way_a_truncated_line_is() {
        let c = parse_setup_choices("enginedir besidedata\nsomethingnewer 42\n\ngarbage\n");
        assert_eq!(c.engine_location, EngineLocation::BesideData);
        assert_eq!(c.patch, PatchChoice::Install);
    }

    #[test]
    fn an_absurd_file_reads_as_nothing_recorded() {
        let t = Tmp::new("huge");
        let mut text = String::from("enginedir besidedata\n");
        while text.len() as u64 <= MAX_SETUP_CHOICES_BYTES {
            text.push_str("# padding padding padding padding padding padding\n");
        }
        std::fs::write(t.file(), text).expect("written");
        assert_eq!(read_setup_choices_from(&t.file()), SetupChoices::default());
    }

    #[test]
    fn the_wire_tokens_round_trip_through_their_own_parsers() {
        for v in [EngineLocation::Applications, EngineLocation::BesideData] {
            assert_eq!(EngineLocation::from_token(v.as_str()), Some(v));
        }
        for v in [PatchChoice::Install, PatchChoice::Skip] {
            assert_eq!(PatchChoice::from_token(v.as_str()), Some(v));
        }
        assert_eq!(EngineLocation::from_token("Applications"), Some(EngineLocation::Applications));
        assert_eq!(PatchChoice::from_token(" SKIP "), Some(PatchChoice::Skip));
        assert_eq!(EngineLocation::from_token(""), None);
        assert_eq!(PatchChoice::from_token("maybe"), None);
    }
}
