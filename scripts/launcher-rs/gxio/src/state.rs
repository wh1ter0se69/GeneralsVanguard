//! Where the LAUNCHER keeps its own configuration and log.
//!
//! This is deliberately not `keybinds_path()`'s directory. That one points at the *engine's*
//! user-data root, because a file the game reads has to live where the game looks. The launcher's
//! own state - which relay list it last fetched, which build it last installed, its log - belongs
//! to the launcher and follows each platform's ordinary per-user convention instead.
//!
//! THE ONE PROPERTY THAT MATTERS, and the reason `smoke-launch.sh --forbid-prefix` exists: this
//! must never resolve to a path that does not survive the run.
//!
//!   * macOS App Translocation relocates a *quarantined* `.app` - which is exactly what a
//!     downloaded, un-notarized bundle is - to a read-only random directory under
//!     `/private/var/folders/…` on first launch. State written beside the executable is
//!     unwritable there, and gone when the user moves the app to /Applications.
//!   * An AppImage started with `--appimage-extract-and-run` unpacks under
//!     `/tmp/appimage_extracted_*` and deletes that tree when the process exits.
//!   * The Windows per-user install directory is replaced wholesale by the next self-update.
//!
//! All three fail on precisely the download every NEW player performs, which is why none of them
//! shows up in ordinary local testing. Anchoring to `$HOME` rather than to the executable is what
//! makes that impossible rather than merely unlikely.

use std::path::PathBuf;

/// The launcher's per-user state directory. `None` only when the platform's home variable is
/// unset, which is the one case where there is genuinely nowhere to write.
///
/// * macOS   - `~/Library/Application Support/GeneralsX`
/// * Windows - `%APPDATA%\GeneralsX`
/// * Linux   - `$XDG_DATA_HOME/GeneralsX`, else `~/.local/share/GeneralsX`
///
/// IN THIS CRATE'S OWN TESTS it is a per-process folder under the temp dir instead, so no test can
/// read or rewrite a player's real state - see `a_test_run_never_resolves_the_real_state_dir`.
pub fn state_dir() -> Option<PathBuf> {
    if cfg!(test) {
        return Some(std::env::temp_dir().join(format!("gxio-test-state-{}", std::process::id())));
    }
    // A test profile's launcher state goes with it (release::user_data_dir_override), never here.
    if let Some(d) = crate::release::user_data_dir_override() {
        return Some(d.join("launcher"));
    }
    user_state_dir()
}

/// The platform's real per-user location - what [`state_dir`] returns outside this crate's tests.
fn user_state_dir() -> Option<PathBuf> {
    #[cfg(target_os = "windows")]
    {
        return std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .map(|p| p.join("GeneralsX"));
    }

    #[cfg(target_os = "macos")]
    {
        return std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library").join("Application Support").join("GeneralsX"));
    }

    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    {
        // XDG first, and only when it is ABSOLUTE. The spec says a relative value must be
        // ignored, and honouring one would put the launcher's state under whatever directory it
        // happened to be started from - which is the AppImage extraction root on a normal run.
        if let Some(x) = std::env::var_os("XDG_DATA_HOME").map(PathBuf::from) {
            if x.is_absolute() {
                return Some(x.join("GeneralsX"));
            }
        }
        return std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join(".local").join("share").join("GeneralsX"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A TEST RUN NEVER RESOLVES THE PLAYER'S REAL STATE DIRECTORY (18/09/2026). Until this, the
    /// suite read and rewrote `~/Library/Application Support/GeneralsX/gxenginestate.txt` on the
    /// machine running it - restoring it afterwards, but rewriting it all the same (measured: the
    /// owner's copy re-stamped at 07:18:48 by a `cargo test`). Every writer this crate has reaches
    /// that folder through `state_dir()`, so pinning `state_dir()` pins all of them.
    #[test]
    fn a_test_run_never_resolves_the_real_state_dir() {
        let d = state_dir().expect("a state dir");
        assert_ne!(Some(&d), user_state_dir().as_ref(), "the tests resolve the player's real state dir");
        assert!(d.starts_with(std::env::temp_dir()), "{} is not under the temp dir", d.display());
    }

    #[test]
    fn state_dir_is_absolute_and_named() {
        let d = user_state_dir().expect("this test host has a home directory");
        assert!(d.is_absolute(), "state_dir must be absolute, got {}", d.display());
        assert_eq!(d.file_name().unwrap(), "GeneralsX");
    }

    /// The guard that the packaging gate is built around: whatever this returns, it is not
    /// under one of the three paths that vanish or turn read-only between runs.
    #[test]
    fn state_dir_survives_the_run() {
        let d = user_state_dir().expect("this test host has a home directory");
        let s = d.to_string_lossy().to_string();
        for forbidden in ["/private/var/folders", "/tmp/appimage_extracted_", "/tmp/.mount_"] {
            assert!(
                !s.starts_with(forbidden),
                "state_dir {s} is under {forbidden}, which does not survive the run"
            );
        }
    }
}
