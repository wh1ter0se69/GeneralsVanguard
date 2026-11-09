// The update state, the changelog and the About page - the three screens that answer "what am I
// running, and is there something newer".
//
// THIS FILE IS GLUE AND NOTHING ELSE. The trust chain is gxcore (`verify` -> `parse` -> `decide`)
// and the I/O is gxio::release; what lives here is the arithmetic that turns one verdict into one
// sentence, and the serde shapes the screens read. That split is not tidiness for its own sake:
// `UpdateStatus.cs` exists because a panel and a Play button once answered the same question from
// two rules that had quietly drifted three ways apart, and the fix was to stop having two rules.
// Nothing below re-decides what would be downloaded.
//
// IT FAILS SAFE, ALWAYS. No network, no game, no cache, a document that does not verify - every
// one of those is a normal state with its own name, and the answer is always "run the installed
// game unchanged" (gxlaunch.c:37). No command here returns an Err a screen is meant to turn into
// a dialog; the Err arm is reserved for "this machine has no home directory", which is the one
// case where there is genuinely nowhere to look.
//
// AND IT NEVER INVENTS A VALUE. Every field below is either measured or empty. A launcher that
// prints a confident wrong build number is worse than one that prints nothing, because the first
// thing a player does with that number is blame a refused join on it - and then go hunting a
// fault that is not there.

use gxio::release as rel;

// ---------------------------------------------------------------------------------------------
// Wire shapes
// ---------------------------------------------------------------------------------------------

/// Can THIS binary install that artifact NAME?
///
/// The macOS arm can only replace an `.app` from a `.tar.gz`, the Linux arm can only replace an
/// AppImage. An engine payload (`GeneralsXZH-macos-arm64-*.zip`, `GeneralsXZH-linux-*.tar.gz`) is a
/// different kind of thing, installed by the FIRST-RUN SETUP page rather than by the updater.
///
/// It does NOT mirror the apply path - it IS the apply path's rule. Both callers reach it through
/// [`applicable_row`]; see the note there for what having two of these cost.
///
/// The NAME is checked as well as the suffix, because the Linux engine payload is itself a
/// `.tar.gz` - a suffix-only rule would call it a launcher the day Linux ships a `.tar.gz` one.
fn applicable_here(name: &str, platform: &str) -> bool {
    if !name.starts_with("GeneralsXLauncher-") {
        return false;
    }
    match platform {
        "macos" => name.ends_with(".tar.gz"),
        "linux" => name.ends_with(".AppImage"),
        _ => true,
    }
}

/// Can this binary install that artifact ROW - the platform column as well as the name?
///
/// THE ONE SELECTOR, and the reason it exists is that there used to be two. The display path grew
/// the name check in fc33f7efd; the apply path kept matching on the platform column and the file
/// suffix alone, so the panel and the irreversible swap answered the same question by two rules.
/// With today's published names they happen to agree - the macOS engine is a `.zip`, and the Linux
/// engine is a `.tar.gz` while the Linux launcher is an `.AppImage`. One repackaging ends that, and
/// what is on the other side of it is `apply_macos_tgz` unpacking an ENGINE payload over the
/// running `.app`. There is no rollback from that step and nothing left to run afterwards, so the
/// two callers share a function rather than a comment asking them to stay in step.
///
/// The platform COLUMN is checked here and not in [`applicable_here`] because `gxcore::decide`
/// hands every platform the `any` rows - the community-patch overlay is one - and an `any` row is
/// nobody's launcher whatever it is called.
pub(crate) fn applicable_row(a: &gxcore::Artifact, platform: &str) -> bool {
    a.platform.eq_ignore_ascii_case(platform) && applicable_here(&a.name, platform)
}

/// Every row of a verdict this binary could actually install - what the update panel counts.
///
/// The apply-side twin is [`select_launcher_artifact`], and both go through [`applicable_row`].
/// Named rather than inlined at the one call site so that "the panel and the swap use one rule" is
/// something a test can hold, which is precisely what was missing when they drifted.
fn applicable_rows(arts: Vec<gxcore::Artifact>, platform: &str) -> Vec<gxcore::Artifact> {
    arts.into_iter().filter(|a| applicable_row(a, platform)).collect()
}

/// The suffix the RUNNING FORM can be replaced from: a macOS `.app` is rebuilt out of a `.tar.gz`,
/// a Linux AppImage is replaced by an `.AppImage`. It is a property of `apply_macos_tgz` /
/// `apply_linux_appimage`, not of the manifest, which is why it is derived from the target and not
/// from the platform token.
#[cfg_attr(target_os = "windows", allow(dead_code))]
fn target_suffix(target: &gxio::update::Target) -> &'static str {
    match target {
        gxio::update::Target::MacApp(_) => ".tar.gz",
        gxio::update::Target::AppImage(_) => ".AppImage",
    }
}

/// The ONE row `release_apply` would hand to the irreversible swap, or `None` to refuse.
///
/// [`applicable_row`] is the same predicate `build_release_state` counts an update with, so a row
/// this returns is always a row the panel listed. The extra suffix test is not redundant with it:
/// `applicable_row` asks what the PLATFORM publishes and this asks what the RUNNING FORM can be
/// rebuilt from, and an extracted-AppImage or translocated-bundle case is exactly where those two
/// stop being the same question.
///
/// `None` is a normal answer, not a fault. The caller turns it into a sentence and installs
/// nothing, which is recoverable; installing the wrong artifact is not.
#[cfg_attr(target_os = "windows", allow(dead_code))]
pub(crate) fn select_launcher_artifact<'a>(
    wanted: &'a [gxcore::Artifact],
    platform: &str,
    target: &gxio::update::Target,
) -> Option<&'a gxcore::Artifact> {
    let suffix = target_suffix(target);
    wanted
        .iter()
        .find(|a| applicable_row(a, platform) && a.name.ends_with(suffix))
}

/// One artifact an update would fetch.
#[derive(serde::Serialize)]
pub struct ReleaseItemDto {
    pub platform: String,
    pub name: String,
    pub size: u64,
    /// A human name for the tier, or the filename when the tier is one this build has no name
    /// for. Never a guess - see `tier_label`.
    pub label: String,
}

/// Everything the two Main screens need to say something true about updates.
///
/// `verdict` is the field to branch on, and `unknown` and `unmanaged` are NOT decoration:
///
/// * `unknown`   - no document could be obtained at all. This must never be phrased as "up to
///                 date"; that phrasing is the exact shape of the bug `UpdateStatus.cs` documents.
/// * `unmanaged` - a document was obtained and verified, but NOTHING ON THIS DISK IDENTIFIES THE
///                 INSTALL: no update record and no `BUILD.txt`. That is a development copy, and
///                 it is the only state left that cannot be related to a release. An install that
///                 merely lacks the RECORD is no longer this - see `installed_from`.
/// * `uptodate`  - the updater's own rule says nothing would download.
/// * `update`    - `items` and `total_bytes` are what would download.
/// * `older`     - the document is older than the install. Nothing downloads either way, but the
///                 UI has to say something different, so the state is kept distinct.
#[derive(serde::Serialize)]
pub struct ReleaseStateDto {
    /// A test profile (GX_USERDATA_DIR) refuses every install, so the page must not hand PLAY over
    /// to an UPDATE that cannot run (Codex r14 of 58d2c9d2d): MainWeb stays, and says so.
    pub installs_off: bool,
    pub verdict: String,
    /// The words beside the status dot, chosen from `verdict` and the evidence by [`headline`].
    /// Both update screens paint this rather than choosing their own.
    pub headline: String,
    /// `network` | `relay` | `cache` | `none` - where the document actually came from.
    pub source: String,
    pub platform: String,
    /// The publish epoch. Handed over raw so the SCREEN formats it: the webview has the player's
    /// locale and timezone and this process does not.
    pub available_ordinal: u64,
    /// Nine hex characters, decoded from the manifest's own signed `sha` field. Empty when no
    /// document was obtained.
    pub available_commit: String,
    /// The publish epoch this install was last updated to, or 0 when nothing measured one. ZERO IS
    /// AN ABSENCE, NOT A DATE - `installed_from` says which, and a screen that formats it must
    /// check it first or it prints January 1970 under "Released".
    pub installed_ordinal: u64,
    /// From `BUILD.txt` if the install has one, else from the update record's artifact names.
    /// Empty when neither exists - which is the honest answer, not `unknown`-as-a-value.
    pub installed_commit: String,
    /// WHAT IDENTIFIES THIS INSTALL, and therefore how much the verdict above could know:
    ///
    /// * `record` - `gxinstalled.txt` is present AND lists something other than the launcher's own
    ///              artifact: an ordinal AND a hash per artifact, which is everything
    ///              `gxcore::decide` wants. The verdict is its unmodified answer. A record holding
    ///              only `GeneralsXLauncher-*` rows - which is every record a macOS or Linux
    ///              self-update writes - describes the LAUNCHER, never the engine, and the engine is
    ///              then identified by the sources below exactly as if there were no record.
    /// * `build`  - no record, but `BUILD.txt` names the commit this install was built from. That
    ///              is enough to say whether this IS the published release, and NOT enough to say
    ///              how far from it: a build stamp carries no ordinal, so `installed_ordinal` is 0
    ///              and stays 0 rather than being invented.
    /// * `binary` - neither file, but the ENGINE ITSELF names the commit it was built from
    ///              (`gxio::enginestamp`). What a development copy carries, and until 18/09/2026
    ///              the launcher never looked: it printed dashes beside a binary that knew its own
    ///              sha. Like `build` it has NO ordinal, so `installed_ordinal` stays 0.
    /// * `installer` - `binary`, AND this launcher's own install vouches for that exact executable:
    ///              its engine record, the stamp it wrote inside the bundle and the executable's
    ///              hash agree (`gxio::engine::launcher_vouches_for`). What a macOS install made by
    ///              this launcher is - it ships no `BUILD.txt` - and the only binary-sourced state
    ///              that may be `uptodate`.
    /// * `none`   - none of the three. The verdict is `unmanaged` and `detail` says why.
    ///
    /// Its own field rather than a second meaning bolted onto `has_record`, which continues to
    /// mean exactly "the update record exists" and nothing else - so `record` implies `has_record`,
    /// and since 18/09/2026 not the reverse (a launcher-only record).
    pub installed_from: String,
    /// When the commit this engine was built from was MADE, RFC 3339 - set only for `binary` and
    /// `installer`.
    ///
    /// THE COMMIT DATE, NOT THE BUILD TIME, and the screen labels it "Committed" for that reason.
    /// The binary's mtime is not a build time either: measured 18/09/2026, the backup engine
    /// `GeneralsXZH.pre-b0b9b5bc8` carries commit `8ff29efa7` (committed 02:04) and an mtime of
    /// 20:42 the same day - it was written when it was moved aside, not when it was linked. The
    /// commit date is the one time in the file that is exactly what it says.
    pub installed_built: String,
    /// The engine the LAUNCHER'S OWN RECORD (`gxenginestate.txt`) says it installed - set only for
    /// `binary`, and only when it DISAGREES with the binary.
    ///
    /// SHOWN, NEVER TRUSTED. The record says what was installed once; the binary is what PLAY will
    /// run. Measured on the owner's Mac 18/09/2026: the record says `8ff29efa7` while the engine on
    /// disk is `3fd38c2da` - a development engine copied over the release by hand, with the release
    /// binary kept beside it as `.pre-b0b9b5bc8`. Preferring the record would print the published
    /// build over a binary that is not it, which is the confidently-wrong value this module exists
    /// to refuse. So the binary wins and the disagreement is reported, not resolved.
    pub recorded_commit: String,
    /// Whether the binary's commit is one NO published release carries. `Some(true)` is what lets
    /// the screen say "Development build" rather than merely "not the published release"; `None`
    /// means no changelog could show it, and then that stronger phrase is not used.
    ///
    /// ONLY A CHANGELOG THIS LAUNCHER FETCHED IN THE LAST [`FETCHED_FRESH_FOR`] CAN SHOW IT
    /// (18/09/2026). An absence is a claim about the document's age: the cached file may predate a
    /// release and omit it, so it is never used. The fetch is `changelog_check`'s own, remembered in
    /// this process - this command never fetches, so the verdict never waits on the changelog. A
    /// list at the parser's `CHANGELOG_MAX_RELEASES` cap may be cut short, and leaves it `None`
    /// too. What no fetch can show: a release published with `--no-changelog`, which never reaches
    /// the changelog at all.
    pub installed_unpublished: Option<bool>,
    /// Who can join this install, as the THIS INSTALL card's "Players can join" row says it - or
    /// EMPTY, and then the row is hidden. See [`who_can_join`]: it is never a sample string, and it
    /// is never a claim the launcher cannot back.
    pub joins: String,
    pub has_record: bool,
    pub items: Vec<ReleaseItemDto>,
    pub total_bytes: u64,
    /// TRUE when `total_bytes` is the launcher's own guess rather than the number the updater that
    /// will perform the download gave. On Windows the estimate comes from `GeneralsXUpdate.exe
    /// --check` (the exact `state_load` + `plan_update` the press runs); when that updater cannot be
    /// asked - missing, too old, or busy with another run - the launcher falls back to its own
    /// `decide`, which reads the same files at a different moment and can name one tier as the whole
    /// if a stale record hides the rest. The page then says the total is being worked out rather
    /// than printing a figure it cannot stand behind. Never true where the launcher IS the updater
    /// (macOS, Linux): there `decide` is exact.
    pub total_estimated: bool,
    /// Where the game is, or empty when no candidate directory held an install marker.
    pub install_dir: String,
    /// One sentence naming why the state is what it is. Written here rather than in the screen so
    /// that the phrasing is one thing rather than four.
    pub detail: String,
}

#[derive(serde::Serialize)]
pub struct ChangelogEntryDto {
    pub ordinal: u64,
    pub commit: String,
    pub date: String,
    pub title: String,
    pub notes: Vec<String>,
    /// True for the entry this install is actually running.
    pub installed: bool,
}

#[derive(serde::Serialize)]
pub struct ChangelogDto {
    /// `network` | `cache` | `none`. `none` with an empty list is the OFFLINE state and is a
    /// normal one - it never blocks Play and never becomes a dialog.
    pub source: String,
    pub entries: Vec<ChangelogEntryDto>,
    /// The next three are [`ReleaseStateDto`]'s fields of the same names, from THE SAME
    /// `installed_identity` call, so the Changelogs screen and the home screen cannot describe one
    /// install two ways (18/09/2026 - before that this screen read the record and `BUILD.txt` only
    /// and never the engine binary). `installed_ordinal` is 0 unless `installed_from` is `record`.
    pub installed_ordinal: u64,
    pub installed_commit: String,
    /// `record` | `build` | `binary` | `installer` | `none`.
    pub installed_from: String,
    /// Whether the marked entry IS this install's release, or only shares its commit - the
    /// screen's INSTALLED against SAME COMMIT. True only when an update record's ORDINAL marked it.
    /// A commit never names one release: the published changelog repeats commits, a cached list
    /// can miss a sibling, and after a launcher-only release the engine's commit sits in the OLDER
    /// release's entry (fifth review, Codex rounds six, eight and nine).
    pub installed_exact: bool,
}

/// The About page's facts. Every string here is measured or empty.
#[derive(serde::Serialize)]
pub struct AboutDto {
    /// This launcher's own version and the commit compiled INTO this binary by build.rs. The
    /// commit is the same value `--gx-selftest` prints and the packaging gate compares, so a
    /// player reading it off this page is reading the gate's own number.
    pub launcher_version: String,
    pub launcher_commit: String,
    pub platform: String,
    pub os: String,
    pub arch: String,

    /// The engine, from BUILD.txt. Empty on a development copy, which has no BUILD.txt.
    pub engine_commit: String,
    pub engine_packaged: String,

    /// The three tier-1 join gates, read out of the engine's own startup line and never
    /// recomputed. Empty until the game has been run at least once - and that emptiness is the
    /// point: a wrong value here reads as "here is why your friend cannot join".
    pub source_id: String,
    pub engine_id: String,
    pub data_id: String,
    /// The log file the three above came from, so the value can be traced. Empty when no run has
    /// been observed.
    pub sim_from: String,

    pub install_dir: String,
    pub state_dir: String,

    /// What is currently published, from the cached document. Empty when none has ever been seen.
    pub available_commit: String,
    pub available_ordinal: u64,
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

/// The update state from what is already on disk. NO NETWORK.
///
/// This is what a screen paints first, so the first frame is never waiting on a CDN.
#[tauri::command(async)]
pub fn release_state() -> Result<ReleaseStateDto, String> {
    Ok(build_release_state(false))
}

/// The update state after asking the publisher. Falls back through exactly the sources the C
/// updater uses, and every one of them fails into the next rather than into a dialog.
#[tauri::command(async)]
pub fn release_check() -> Result<ReleaseStateDto, String> {
    Ok(build_release_state(true))
}

/// The release notes from the local cache. NO NETWORK.
#[tauri::command(async)]
pub fn changelog_state() -> Result<ChangelogDto, String> {
    Ok(build_changelog(false))
}

/// The release notes, refreshed from the publisher.
#[tauri::command(async)]
pub fn changelog_check() -> Result<ChangelogDto, String> {
    Ok(build_changelog(true))
}

/// The Tauri event `release_apply` streams progress on: payload `{done, total}`, the whole update's
/// bytes - on Windows per `progress:` line the updater prints, on macOS and Linux from the launcher's
/// own downloads.
pub const UPDATE_PROGRESS_EVENT: &str = "gx-update-progress";

/// Apply the pending update - UPDATE on MainUpdate - streaming its progress to the page, per
/// platform: on macOS the engine install and the launcher's own update (its arm below says how), on
/// Linux the launcher's AppImage, and on Windows the C updater, as follows.
///
/// ON WINDOWS the update PIPELINE is `GeneralsXUpdate.exe`: the component that fetches, verifies and
/// swaps artifacts, and it exists only there. This command adds no second updater - it spawns the proven one with `--no-launch` (apply, do not start the game), cwd at
/// the install dir, and speaks its stdout contract: `progress: <done> <total>` lines become
/// [`UPDATE_PROGRESS_EVENT`] emissions, and the exact line `restart: launcher` means the runtime
/// tier replaced THIS binary, so the caller should offer `relaunch`. The contract lives in
/// `gxio::release::parse_updater_line`, where it has tests.
///
/// Exit 0 -> `Ok({"restart_needed": bool})`. Anything else -> Err carrying the last line the
/// updater printed, which is where it states its reason.
#[tauri::command(async)]
pub fn release_apply(app: tauri::AppHandle) -> Result<serde_json::Value, String> {
    gxio::release::refuse_install_on_test_profile()?;
    #[cfg(target_os = "windows")]
    {
        use std::io::BufRead;
        use tauri::Emitter;

        let (dir, exe) = updater_exe()?;

        let mut cmd = std::process::Command::new(&exe);
        cmd.arg("--no-launch")
            .current_dir(&dir)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null());
        {
            // No console flash - same rule as every helper process the launcher starts.
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(gxio::game::CREATE_NO_WINDOW);
        }

        let mut child = cmd
            .spawn()
            .map_err(|e| format!("could not start {}: {e}", exe.display()))?;
        let stdout = child
            .stdout
            .take()
            .ok_or("the updater's output could not be captured")?;

        let mut restart_needed = false;
        let mut last_line = String::new();
        for line in std::io::BufReader::new(stdout).lines() {
            let Ok(line) = line else { break };
            let line = line.trim_end().to_string();
            if line.is_empty() {
                continue;
            }
            match rel::parse_updater_line(&line) {
                rel::UpdaterLine::Progress { done, total } => {
                    // Best effort, like every emit in this launcher: a page with no listener is
                    // normal, and the apply must not fail over its progress bar.
                    let _ = app.emit(
                        UPDATE_PROGRESS_EVENT,
                        serde_json::json!({ "done": done, "total": total }),
                    );
                }
                rel::UpdaterLine::RestartLauncher => restart_needed = true,
                rel::UpdaterLine::Other => {}
            }
            last_line = line;
        }

        let status = child
            .wait()
            .map_err(|e| format!("the updater could not be waited on: {e}"))?;
        if status.success() {
            Ok(serde_json::json!({ "restart_needed": restart_needed }))
        } else {
            Err(format!(
                "The updater failed ({}){}",
                status,
                if last_line.is_empty() {
                    " with no output.".to_string()
                } else {
                    format!(" — last output: {last_line}")
                }
            ))
        }
    }

    // GeneralsX @feature Claude 28/09/2026 macOS: UPDATE RUNS THE TWO INSTALLS THIS LAUNCHER ALREADY HAS.
    //
    // The owner: "macOS launcher should behave just the same as windows going yellow for an update
    // button". A pending macOS update now hands over to MainUpdate like Windows, and this is what its
    // UPDATE does - no second updater: the ENGINE through `cmd_setup::install_engine_reporting` (the
    // setup page's install, whole: claim, bundle lock, running-game refusal, hash, atomic swap, the
    // player's files carried), then the LAUNCHER through `self_update::run_pressed` (the swap the
    // background job used to make: exchange-only, the new launcher must answer). Engine first: the
    // launcher's swap waits for the bundles an engine install holds, and a new launcher relaunched
    // mid-install would kill it.
    //
    // WHAT IS OUTSTANDING is the verdict's own rule, asked of the same document: the engine row
    // `cmd_setup::engine_update_row` lists and `launcher_outstanding`. ONE BAR for both: `done` counts
    // the engine's bytes and then the launcher's on top, against their sum, so the Update page's meter
    // (gx-progress.js) never sees its total change or its count step back between the two.
    //
    // ANY PIECE NOT APPLIED IS AN Err, with its sentence and whether the engine got there first -
    // never an Ok that leaves UPDATE offering the same thing again. MainUpdate then shows it and its
    // way back to PLAY. `restart_needed` only when the launcher itself was swapped.
    #[cfg(target_os = "macos")]
    {
        use tauri::Emitter;

        let installed = rel::read_install_record()
            .as_ref()
            .map(|(_, t)| gxcore::InstallState::parse(t))
            .unwrap_or_default();
        // A fixture build run with its fixture presses against the fixture's document and identity,
        // exactly as its verdict was judged (self_update::fixture_world, Codex rel0949-r2).
        let fixture = crate::self_update::fixture_world();
        let (manifest, _source) = match fixture {
            Some((m, _)) => (Some(m.clone()), "fixture"),
            None => choose_manifest(true, installed.ordinal),
        };
        let own = fixture.map_or_else(crate::self_update::own_build, |(_, own)| own);
        let m = manifest.ok_or(
            "No verified release document is available, so there is nothing to apply. Check your \
             connection and press Check again.",
        )?;
        if m.ordinal < installed.ordinal {
            return Err(format!(
                "The published release ({}) is older than what is installed ({}), so applying it \
                 would move this launcher backwards.",
                m.ordinal, installed.ordinal
            ));
        }
        let platform = rel::platform_token();
        let engine = crate::cmd_setup::engine_update_row(&m, platform, gxio::game::discover().install_dir.is_some());
        let launcher = launcher_outstanding(&m, platform, own, Some(&installed), false);
        if engine.is_none() && launcher.is_none() {
            return Err("Nothing newer is published for this install.".to_string());
        }

        let engine_size = engine.as_ref().map_or(0, |a| a.size);
        let total = engine_size + launcher.as_ref().map_or(0, |a| a.size);
        let bar = |done: u64| {
            // Best effort, like every emit in this launcher: a page with no listener is normal.
            let _ = app.emit(
                UPDATE_PROGRESS_EVENT,
                serde_json::json!({ "done": done.min(total), "total": total }),
            );
        };
        if engine.is_some() {
            crate::cmd_setup::install_engine_reporting(Some(&m), &mut |received, _| bar(received.min(engine_size)))?;
            bar(engine_size);
        }
        let Some(_) = launcher else {
            return Ok(serde_json::json!({ "restart_needed": false }));
        };
        let outcome = crate::self_update::run_pressed(&app, &m, installed, &|done| bar(engine_size + done));
        use crate::self_update::SelfUpdateOutcome as O;
        match outcome {
            O::Installed { .. } => Ok(serde_json::json!({ "restart_needed": true })),
            O::Current { .. } => Ok(serde_json::json!({ "restart_needed": false })),
            O::Nothing(why) | O::Unavailable(why) | O::Deferred(why) | O::Failed(why) => Err(if engine.is_some() {
                format!("GeneralsX itself is updated; the launcher's own update did not install: {why}")
            } else {
                why
            }),
        }
    }

    // GeneralsX @feature Claude 25/08/2026 LINUX CAN APPLY AN UPDATE (and macOS could, until
    // 28/09/2026 gave it the arm above).
    //
    // This arm used to return "Updates cannot be applied on this platform yet". Everything it
    // needed already existed in `gxio::update` - locate_target, download_to_file, read_chunks,
    // apply_macos_tgz, apply_linux_appimage, staging_dir - with 41 tests and NO CALLER. The
    // pipeline was built and never connected.
    //
    // WHAT THIS REPLACES IS THE LAUNCHER, NOT THE GAME. The manifest's macos/linux rows are the
    // launcher itself; there is no macOS or Linux ENGINE row in it at all, which is why the
    // website still hands those two platforms a direct engine download. Do not read this commit
    // as closing that gap - it closes the launcher half only.
    //
    // ORDER IS THE WHOLE DESIGN. Refuse before spending anything (locate_target reads this
    // process's own path and fails on a translocated bundle or an extracted AppImage), then
    // download to staging, then VERIFY, and only then swap. The swap is the one irreversible step
    // and nothing reaches it unhashed.
    #[cfg(not(any(target_os = "windows", target_os = "macos")))]
    {
        use tauri::Emitter;

        // Refuses on a translocated .app, an extracted AppImage, or an install this user cannot
        // write - all discovered from our own path, before a byte is fetched.
        let target = gxio::update::locate_target()?;

        let record = rel::read_install_record();
        let installed = record
            .as_ref()
            .map(|(_, t)| gxcore::InstallState::parse(t))
            .unwrap_or_default();

        // Network allowed: the player pressed Update. `choose_manifest` owns the trust chain -
        // this function must never verify a document a second way.
        let (manifest, _source) = choose_manifest(true, installed.ordinal);
        let m = manifest.ok_or(
            "No verified release document is available, so there is nothing to apply. Check your \
             connection and press Check again.",
        )?;

        let platform = rel::platform_token();
        if m.ordinal < installed.ordinal {
            return Err(format!(
                "The published release ({}) is older than what is installed ({}), so applying it \
                 would move this launcher backwards.",
                m.ordinal, installed.ordinal
            ));
        }
        // THE SAME RULE THE SCREENS SHOW (`launcher_outstanding`), judged against this running
        // binary - not the install record's hash, which is what let a stale record offer, and this
        // path then install, a launcher the rest of the launcher called current.
        let wanted: Vec<gxcore::Artifact> =
            launcher_outstanding(&m, platform, crate::self_update::own_build(), Some(&installed), matches!(target, gxio::update::Target::AppImage(_))).into_iter().collect();
        if wanted.is_empty() {
            return Err("No newer launcher is published for this one.".to_string());
        }

        // `launcher_outstanding` already names one launcher row; it still goes through
        // `select_launcher_artifact`, the SAME predicate the display path counts with, because that
        // also checks the row against the RUNNING FORM (an .app from a .tar.gz, an AppImage from an
        // .AppImage) - applying anything else through the swap would be a category error.
        let suffix = target_suffix(&target);
        let art = select_launcher_artifact(&wanted, platform, &target).ok_or_else(|| {
            format!(
                "The published release names no {platform} launcher artifact ending in \
                 {suffix}, so there is nothing here this launcher can install."
            )
        })?;

        let staging = gxio::update::staging_dir().ok_or(
            "The launcher could not create its staging directory, so it cannot download an \
             update safely.",
        )?;
        let dest = staging.join(&art.name);

        // `base` carries no scheme and ends in '/', e.g. "dl.wh1ter0se.dev/". download_to_file
        // refuses anything that is not https://.
        let url = format!("https://{}{}", m.base, art.name);
        let mut on_progress = |done: u64, total: u64| {
            // Best effort, exactly like the Windows arm: a page with no listener is normal and the
            // apply must not fail over its progress bar.
            let _ = app.emit(
                UPDATE_PROGRESS_EVENT,
                serde_json::json!({ "done": done, "total": total }),
            );
        };
        gxio::update::download_to_file(&url, &dest, art.size, &mut on_progress)?;

        // VERIFY BEFORE SWAPPING. Extracted so the ordering is testable without a Tauri handle -
        // see `a_hash_mismatch_is_refused_and_the_file_is_removed`.
        gxio::update::verify_staged(&dest, &art.name, &art.hash)?;

        match &target {
            gxio::update::Target::MacApp(app_path) => {
                gxio::update::apply_macos_tgz(&dest, app_path)?
            }
            gxio::update::Target::AppImage(path) => {
                gxio::update::apply_linux_appimage(&dest, path)?
            }
        }
        let _ = std::fs::remove_file(&dest);

        // RECORD WHAT WAS APPLIED, OR THIS UPDATE IS OFFERED AGAIN FOREVER.
        //
        // GeneralsX @bugfix Claude 25/08/2026 The apply path used to end here. `build_release_state`
        // decides from `read_install_record()` and, failing that, from the engine's `BUILD.txt` -
        // and a launcher-only update moves NEITHER, because BUILD.txt rides the engine tier and
        // nothing on Unix has ever written a record. So the verdict after a successful apply was
        // byte-identical to the verdict before it: press Update, watch it succeed, be told an
        // update is available. That is the 12/08 permanent-re-download defect in a new place.
        //
        // Only the artifact that was ACTUALLY applied is recorded. The C updater rebuilds the whole
        // record from the manifest (gxlaunch.c:2168-2181), which is truthful there because it
        // applies every needed artifact; this arm applies exactly one. Claiming the rest would tell
        // `decide` that artifacts nobody installed are up to date, and they would never be fetched.
        //
        // The ordinal is raised because it is a rollback FLOOR, not a completeness claim -
        // `decide` still filters every artifact by hash, so anything not applied stays needed.
        // `max` so a record can never move backwards.
        let target_path: &std::path::Path = match &target {
            gxio::update::Target::MacApp(p) => p,
            gxio::update::Target::AppImage(p) => p,
        };
        let state = record_one_applied(installed, art, m.ordinal);

        // A failure here is NOT an apply failure - the new launcher is already installed and
        // running it is the right thing to do. It does mean the update will be offered again, so
        // it is worth saying so rather than swallowing it.
        let recorded = match rel::install_record_target(Some(target_path)) {
            Some(path) => match rel::write_install_record(&path, &state.render()) {
                Ok(()) => true,
                Err(e) => {
                    eprintln!("gx: the update was applied but its record could not be written to {} ({e}); it will be offered again", path.display());
                    false
                }
            },
            None => {
                eprintln!("gx: the update was applied but there is nowhere to write its record; it will be offered again");
                false
            }
        };

        // Always true here: this arm replaces the running launcher, so the freshly installed copy
        // is not the one executing. The Windows arm learns this from the updater's own output.
        Ok(serde_json::json!({ "restart_needed": true, "recorded": recorded }))
    }
}

/// Hash a staged download against the signed release, and delete it if it does not match.
///
/// Separate from [`release_apply`] for one reason: the property that matters here is an ORDERING -
/// nothing reaches the swap unhashed - and an ordering is only worth asserting if a test can
/// assert it. `release_apply` needs a `tauri::AppHandle`, so it cannot be called from a unit test;
/// this can.
///
/// Streamed rather than read whole. The Linux AppImage is ~78 MB, and a caller forced to choose
/// between that allocation and skipping the check will skip the check - under exactly the memory
/// pressure that truncates downloads in the first place.
///
/// The file is REMOVED on mismatch. Leaving it in staging would let the next attempt find a
/// plausible-looking artifact of the right name and skip the download; `prune_staging` clears it
/// eventually, but "eventually" is after the retry the player is about to press.
#[cfg_attr(target_os = "windows", allow(dead_code))]
/// Fold ONE applied artifact into the install record.
///
/// Extracted so the claim this whole fix rests on is testable without a Tauri handle, a network or
/// a real swap: after a successful apply, `gxcore::decide` must stop returning `Update` for the
/// artifact that was applied - and must keep returning it for every artifact that was not.
///
/// `ordinal` is raised, never lowered: it is `decide`'s rollback floor, not a claim that everything
/// in the manifest is installed. Per-artifact hashes still gate everything else.
#[cfg_attr(target_os = "windows", allow(dead_code))]
pub(crate) fn record_one_applied(
    mut state: gxcore::InstallState,
    applied: &gxcore::Artifact,
    ordinal: u64,
) -> gxcore::InstallState {
    state.ordinal = state.ordinal.max(ordinal);
    match state
        .items
        .iter_mut()
        .find(|i| i.platform.eq_ignore_ascii_case(&applied.platform) && i.name == applied.name)
    {
        Some(existing) => existing.hash = applied.hash.clone(),
        None => state.items.push(gxcore::InstalledItem {
            platform: applied.platform.clone(),
            name: applied.name.clone(),
            hash: applied.hash.clone(),
        }),
    }
    state
}

// `verify_staged` used to live here as a private fn. It is now `gxio::update::verify_staged`,
// lifted so `gxio::engine::install_engine` can call the SAME hash check this launcher's own
// self-update uses instead of a second one that could quietly disagree with it - see that
// function's doc comment. Every call site below was updated to the new home; nothing about the
// check itself changed.

/// Restart the launcher - offered after `release_apply` reports `restart_needed`, so the freshly
/// replaced binary is the one that starts.
///
/// R-8: IT WAITS FOR AN ENGINE INSTALL FIRST. `AppHandle::restart` ends this process exactly as
/// `exit` does, and the engine install runs inside it, so an automatic relaunch killed an in-flight
/// install in precisely the way PLAY did. `cmd_game::restart_in_background` takes the same claim
/// PLAY's closer takes and holds it across the restart; this command returns at once, and
/// `MainUpdate.html` already paints its own inert "restarting" state while it waits.
#[tauri::command]
pub fn relaunch(app: tauri::AppHandle) {
    crate::cmd_game::restart_in_background(app);
}

/// The About page. NO NETWORK - it reads this binary's own stamps, the install, and the engine's
/// log.
#[tauri::command(async)]
pub fn about_info() -> Result<AboutDto, String> {
    // The same per-platform rule the THIS INSTALL panel uses - see `engine_build_stamp`.
    let stamp = rel::engine_build_stamp();
    let sim = rel::read_sim_id().unwrap_or_default();
    let published = cached_manifest();

    Ok(AboutDto {
        launcher_version: env!("CARGO_PKG_VERSION").to_string(),
        launcher_commit: env!("GX_COMMIT9").to_string(),
        platform: rel::platform_token().to_string(),
        os: std::env::consts::OS.to_string(),
        arch: std::env::consts::ARCH.to_string(),

        // BUILD.txt first, then what the ENGINE BINARY says about itself - the same order and the
        // same source the THIS INSTALL panel uses, so About and Main cannot show one install two
        // ways. Until 18/09/2026 this read BUILD.txt alone and printed a dash on every development
        // copy, beside an engine that carried its own sha.
        engine_commit: stamp
            .as_ref()
            .map(|s| rel::short_commit(&s.commit))
            .filter(|c| !c.is_empty())
            .or_else(|| {
                gxio::enginestamp::find_engine_stamp().map(|(_, s)| rel::short_commit(&s.commit))
            })
            .unwrap_or_default(),
        engine_packaged: stamp.as_ref().map(|s| s.packaged.clone()).unwrap_or_default(),

        source_id: sim.source,
        engine_id: sim.engine,
        data_id: sim.data,
        sim_from: sim.from,

        // The marker search first, then DISCOVERY - what PLAY runs and what Main's "Installed to"
        // already shows. The marker search looks for a FILE named like the engine, which a macOS
        // `.app` bundle is not, so on a bundle install it found nothing and About printed a dash
        // for a path Main was displaying at the same moment.
        install_dir: rel::find_install_dir()
            .or_else(|| gxio::game::discover().install_dir)
            .map(|p| p.display().to_string())
            .unwrap_or_default(),
        state_dir: gxio::state_dir()
            .map(|p| p.display().to_string())
            .unwrap_or_default(),

        available_commit: published
            .as_ref()
            .and_then(|m| rel::commit_hex_from_sha_field(&m.sha))
            .map(|h| rel::short_commit(&h))
            .unwrap_or_default(),
        available_ordinal: published.as_ref().map(|m| m.ordinal).unwrap_or(0),
    })
}

// ---------------------------------------------------------------------------------------------
// The manifest, from the best source available
// ---------------------------------------------------------------------------------------------

/// Verify and parse a wire document. The two halves are never separable: `parse` takes a
/// `Verified`, so there is no route to the fields that skips the signature.
fn verified(wire: &str) -> Option<gxcore::Manifest> {
    let v = gxcore::verify(wire).ok()?;
    gxcore::parse(&v).ok()
}

/// The cached document, re-verified on load.
///
/// NEVER TRUSTED BECAUSE IT IS LOCAL. A file on this disk is not more trustworthy than the
/// network, it is only closer - and it was written by a previous run of something, which is not
/// the same as being written by the publisher.
pub(crate) fn cached_manifest() -> Option<gxcore::Manifest> {
    let (_, text) = rel::read_cached(rel::MANIFEST_CACHE, rel::MANIFEST_MAX_BYTES)?;
    match rel::http_document(&text) {
        rel::Framed::Document(d) => verified(d),
        _ => None,
    }
}

/// The best verified manifest available, fetching one if the cache has none - the setup screen's
/// entry point into the same trust chain [`release_check`] uses.
///
/// WHY THE SETUP PAGE NEEDS THIS AND NOT JUST [`cached_manifest`]. `describe_engine_payload` read
/// the cache only, so on a genuine first run - a machine that has never held `gxmanifest.txt` -
/// it answered "no release document has been fetched yet", hid the INSTALL control, and never
/// refreshed when the background fetch behind `release_check` landed. That is precisely the state
/// this whole feature exists for. Reported by the cross-model review, 10/09/2026.
///
/// `choose_manifest` is what is called, not a second fetch written here: it owns the rollback
/// floor (an authentic but OLDER document is refused), the network-then-relay fallback, and the
/// cache write. Nothing on this path verifies a document a second way.
pub(crate) fn fetched_manifest() -> Option<gxcore::Manifest> {
    let installed = rel::read_install_record()
        .as_ref()
        .map(|(_, t)| gxcore::InstallState::parse(t))
        .unwrap_or_default();
    choose_manifest(true, installed.ordinal).0
}

/// Where a document came from, and what it was.
struct Fetched {
    manifest: gxcore::Manifest,
    source: &'static str,
    /// The exact bytes that verified, kept so the cache stores what was SIGNED rather than
    /// something reconstructed from the parsed fields.
    wire: String,
}

/// Ask the publisher over HTTPS.
fn fetch_published() -> Option<Fetched> {
    let body = rel::fetch_https(rel::MANIFEST_URL, rel::MANIFEST_MAX_BYTES)?;
    let rel::Framed::Document(d) = rel::http_document(&body) else {
        // A 404 page or a truncated upload. Deliberately NOT reported as a signature failure.
        return None;
    };
    let m = verified(d)?;
    Some(Fetched { manifest: m, source: "network", wire: d.to_string() })
}

/// Ask a relay over UDP - the last source, and the only one an install that predates the HTTPS
/// bootstrap has ever had.
///
/// Relays come from the cached document if there is one, and otherwise from the address the game
/// itself is pointed at. `GXMANI -` is an ANSWER, not a failure: it means that relay is serving no
/// manifest, and the caller moves on rather than reporting a broken signature.
fn fetch_from_relay(cached: Option<&gxcore::Manifest>, installed_ordinal: u64) -> Option<Fetched> {
    let mut hosts: Vec<(String, u16)> = Vec::new();
    if let Some(m) = cached {
        for r in &m.relays {
            hosts.push((r.host.clone(), r.port.unwrap_or(rel::RELAY_LOBBY_PORT)));
        }
    }
    if hosts.is_empty() {
        if let Some(a) = rel::read_relay_address() {
            hosts.push(a);
        }
    }

    for (host, port) in hosts {
        let Some(reply) = rel::fetch_relay(&host, port, installed_ordinal) else {
            continue;
        };
        match rel::strip_relay_framing(&reply) {
            rel::Framed::Document(d) => {
                if let Some(m) = verified(d) {
                    return Some(Fetched {
                        manifest: m,
                        source: "relay",
                        wire: d.to_string(),
                    });
                }
            }
            // This relay has nothing. Not an error, not a signature failure - try the next one.
            rel::Framed::NothingToServe => continue,
            rel::Framed::Unexpected => continue,
        }
    }
    None
}

/// Pick the document to act on.
///
/// THE CACHED ORDINAL IS A FLOOR, and this is the one rule here that is about security rather
/// than about phrasing. A signature says WHO wrote a document; it says nothing about WHEN. So
/// anyone who decides which bytes this machine receives - a CDN edge holding a stale object, an
/// operator error, a compromised bucket - can serve an older but perfectly authentic document. If
/// that were taken, it would walk the install backwards and overwrite the newer cached copy on the
/// way. The fact that such a rollback is built entirely out of authentic signed bytes is exactly
/// what makes refusing it worth the code. Same rule as `bootstrap_manifest`, gxlaunch.c:1069-1082.
/// GeneralsX @bugfix Claude 29/10/2026 S1-3 (Codex relaylist-v2-r7) ONE choice at a time in this process. Tauri runs
/// commands on a thread pool, so `release_check` and `setup_state_checked` can overlap: both read the same cached
/// document, and the slower one - comparing its fetch against that stale snapshot - could overwrite a NEWER document the
/// faster one had just cached, undoing the serial floor and key retirement. The whole read-fetch-compare-write runs under
/// this lock (a poisoned lock is still taken: the cache file, not memory, is the state).
static MANIFEST_CHOICE: std::sync::Mutex<()> = std::sync::Mutex::new(());

pub(crate) fn choose_manifest(allow_network: bool, installed_ordinal: u64) -> (Option<gxcore::Manifest>, &'static str) {
    let _choice = MANIFEST_CHOICE.lock().unwrap_or_else(|e| e.into_inner());
    let cached = cached_manifest();
    // S1-3 (Codex relaylist-v2-r3): the held document's ACCEPTANCE time is its cache file's write time - the file is
    // written only when a document is accepted (an identical re-fetch below is not rewritten), and a clock change
    // does not move it. The cached text is kept to recognise that re-fetch.
    let (cached_text, cached_at) = match rel::read_cached(rel::MANIFEST_CACHE, rel::MANIFEST_MAX_BYTES) {
        Some((p, t)) => {
            let at = std::fs::metadata(&p).and_then(|m| m.modified()).ok()
                .and_then(|m| m.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs());
            (Some(t), at)
        }
        None => (None, None),
    };

    if !allow_network {
        let src = if cached.is_some() { "cache" } else { "none" };
        return (cached, src);
    }

    let got = fetch_published().or_else(|| fetch_from_relay(cached.as_ref(), installed_ordinal));

    let Some(f) = got else {
        let src = if cached.is_some() { "cache" } else { "none" };
        return (cached, src);
    };

    if let Some(c) = &cached {
        // GeneralsX @feature Claude 29/10/2026 S1-3: rank first, then the ordinal floor, then the serial at an
        // equal ordinal (gxcore::supersedes). A higher-ranked document is taken even with an older ordinal: a
        // stolen K1 that poisoned the ordinal floor must not block the K2 recovery document.
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        if !gxcore::supersedes(&f.manifest, c, now, cached_at.unwrap_or(now)) {
            // Older than what we already hold, or from a retired key rank. Keep the cached one AND do not
            // overwrite it.
            return (cached, "cache");
        }
    }

    // Cache the bytes that verified. A failed write costs the next offline launch and nothing
    // else, so it is not reported as an error. Not when they ARE the cached document: its write time is its
    // acceptance time, which the serial horizon above is judged on.
    let same = cached_text.as_deref()
        .map(|t| matches!(rel::http_document(t), rel::Framed::Document(d) if d == f.wire))
        .unwrap_or(false);
    if !same {
        // The write time is the acceptance HIGH-WATER mark the horizon judges the held serial on: never moved BACK
        // (Codex relaylist-v2-r8) - a document accepted on a wound-back clock is stamped max(now, the previous time).
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        let at = now.max(cached_at.unwrap_or(0));
        if let Ok(p) = rel::write_cache_atomic(rel::MANIFEST_CACHE, &f.wire) {
            let _ = std::fs::OpenOptions::new().write(true).open(&p)
                .and_then(|fh| fh.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(at)));
        }
    }
    (Some(f.manifest), f.source)
}

// ---------------------------------------------------------------------------------------------
// The update state
// ---------------------------------------------------------------------------------------------

fn build_release_state(allow_network: bool) -> ReleaseStateDto {
    let record = rel::read_install_record().map(|(_, t)| gxcore::InstallState::parse(&t));
    // NOTHING IS FETCHED FOR THIS. "Development build" rests on a changelog the Changelogs half
    // (`changelog_check`, which every screen calls beside this one) fetched a moment ago - see
    // `installed_unpublished` for why a cached copy may not decide it.
    let identity = installed_identity(record.as_ref(), || release_list_for_absence(fetched_releases()));
    // The record's ordinal is the rollback floor for choosing a document, launcher row or not.
    // A fixture build run with its fixture judges the fixture's document instead (self_update::fixture_world).
    let fixture = crate::self_update::fixture_world();
    let (manifest, source) = match fixture {
        Some((m, _)) => (Some(m.clone()), "fixture"),
        None => choose_manifest(allow_network, record.as_ref().map(|r| r.ordinal).unwrap_or(0)),
    };
    // macOS: the engine bundle discovery finds - what PLAY runs, and what MainWeb's "Installed to"
    // names. The verdict's engine rule reads it, and so does MainUpdate's "Installed to", which
    // `find_install_dir` (install markers, the Windows and Linux layouts) never finds in a bundle.
    let mac_engine = (rel::platform_token() == "macos").then(|| gxio::game::discover().install_dir).flatten();
    release_view(
        manifest.as_ref(),
        source,
        allow_network,
        &Disk {
            platform: rel::platform_token(),
            record: record.as_ref(),
            identity: &identity,
            build_commit: rel::engine_build_stamp()
                .map(|s| rel::short_commit(&s.commit))
                .unwrap_or_default(),
            own: fixture.map_or_else(crate::self_update::own_build, |(_, own)| own),
            appimage: matches!(gxio::update::locate_target(), Ok(gxio::update::Target::AppImage(_))),
            // macOS only: the one platform whose launcher installs the engine (`engine_install_dir`).
            engine: if rel::platform_token() == "macos" {
                manifest.as_ref().and_then(|m| crate::cmd_setup::engine_update_row(m, "macos", mac_engine.is_some()))
            } else {
                None
            },
            install_dir: mac_engine
                .clone()
                .or_else(rel::find_install_dir)
                .map(|p| p.display().to_string())
                .unwrap_or_default(),
            // Ask the component that will do the download what it will download - but ONLY on
            // the network-enabled refresh (`release_check`).
            //
            // THE DISK-ONLY PATH DOES NOT SYNTHESIZE A WIN_PLAN-SHAPED VERDICT AT ALL. This is the
            // owner's decision, not a bug fix layered on the last one: two rounds tried to make an
            // APPROXIMATION of the real plan less wrong on this path - first "call it every time"
            // (blocks up to 60s behind the updater's own HTTPS bootstrap), then "cache the last
            // real answer" (a stale record vs. an empty real plan ping-pongs MainWeb/MainUpdate,
            // `an_empty_updater_plan_downgrades_a_derived_update`'s exact shape), then "key the
            // cache to the install record" (fixed that, but a completed update still needed a
            // stale plan around to be able to lie about). Each fix removed one leak of the same
            // approximation; the owner chose to stop approximating instead of finding leak four.
            //
            // `None` here sends `reconcile_plan` straight to `decide`'s own guess - already
            // labelled `total_estimated` on Windows, so the page reads "Calculating..." rather
            // than a number it cannot stand behind. That guess is a pure function of the CURRENT
            // manifest and the CURRENT install record (`rel::read_install_record`, read fresh on
            // every call, no memo) - which is exactly why this also fixes the completed-update
            // case for free: a successful `release_apply` rewrites the record before `MainUpdate`
            // calls `release_state` again, so the very next guess is computed against the NEW
            // install and agrees with it. A real, priced answer appears once `release_check` has
            // actually asked the updater in THIS call - never a memory of an earlier one.
            win_plan: if allow_network { updater_check_plan() } else { None },
        },
    )
}

/// The install folder and its `GeneralsXUpdate.exe`, for [`release_apply`] and [`updater_check_plan`].
///
/// GeneralsX @bugfix Claude 28/09/2026 NEVER UNDER A TEST PROFILE (Codex r2 of 2aa151364, P1). The C
/// updater (gxlaunch.c) does not follow GX_USERDATA_DIR: it reads the owner's Options.ini and
/// GXLogin.txt and writes RelayAddress into that Options.ini, so running it - even `--check` - from a
/// launcher on a test profile would reach the profile the override exists to keep out of reach.
#[cfg(target_os = "windows")]
fn updater_exe() -> Result<(std::path::PathBuf, std::path::PathBuf), String> {
    if gxio::release::user_data_dir_override().is_some() {
        return Err("Updates are off on a test profile (GX_USERDATA_DIR): GeneralsXUpdate.exe would use the \
                    owner's profile. Unset it to update."
            .to_string());
    }
    let dir = rel::find_install_dir()
        .ok_or("No install was found on this machine, so there is no updater to run.")?;
    let exe = dir.join("GeneralsXUpdate.exe");
    if !exe.is_file() {
        return Err(format!("{} is not there. Reinstall to restore the updater.", exe.display()));
    }
    Ok((dir, exe))
}

/// Ask `GeneralsXUpdate.exe --check` for the plan it would run RIGHT NOW - the same `state_load` +
/// `plan_update` the press performs, so the launcher's "Will download" is the plan and not a second
/// reading of the same files that can disagree with it.
///
/// Windows only: the C updater is the download component and exists only there. `None` when there is
/// no updater on disk, it is too old to print a plan, it is busy with another run, or it could not be
/// started - every one of which means "fall back and say the total is being worked out", never "name
/// one tier as the whole".
#[cfg(target_os = "windows")]
fn updater_check_plan() -> Option<gxio::release::UpdaterPlan> {
    let (dir, exe) = updater_exe().ok()?;
    let mut cmd = std::process::Command::new(&exe);
    cmd.arg("--check")
        .current_dir(&dir)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null());
    {
        // No console flash - same rule as `release_apply` and every helper the launcher starts.
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(gxio::game::CREATE_NO_WINDOW);
    }
    let out = cmd.output().ok()?;
    rel::parse_updater_plan(&String::from_utf8_lossy(&out.stdout))
}

#[cfg(not(target_os = "windows"))]
fn updater_check_plan() -> Option<gxio::release::UpdaterPlan> {
    None
}

/// What `build_release_state` read off this machine - everything [`release_view`] needs besides
/// the document.
struct Disk<'a> {
    platform: &'a str,
    /// The parsed `gxinstalled.txt`, or `None` when there is none.
    record: Option<&'a gxcore::InstallState>,
    identity: &'a Identity,
    /// `BUILD.txt`'s commit, short, through the per-platform rule - or empty.
    build_commit: String,
    /// The running launcher.
    own: crate::self_update::OwnBuild<'a>,
    /// The running launcher is an AppImage this process could replace (`gxio::update::locate_target`,
    /// the check `release_apply` makes before it downloads anything).
    appimage: bool,
    /// The ENGINE an UPDATE press would install here - macOS only, `cmd_setup::engine_update_row`:
    /// an engine is discovered, and the published one is newer or it is not one this launcher
    /// installed. `None` everywhere else, and on Windows the engine is `decide`'s own row.
    engine: Option<gxcore::Artifact>,
    install_dir: String,
    /// THE UPDATER'S OWN PLAN, on Windows, from `GeneralsXUpdate.exe --check` - the authoritative
    /// answer to "what will the press download". `None` off Windows (there the launcher is the
    /// updater), `None` on Windows when the updater could not be asked, and ALWAYS `None` on the
    /// disk-only path (`release_state`), which never asks - see the comment on this field's only
    /// call site. See [`ReleaseStateDto::total_estimated`].
    win_plan: Option<gxio::release::UpdaterPlan>,
}

/// THE PURE HALF of `build_release_state`: every word and number the update screens show, from what
/// was read. The truth-table tests drive THIS, against the real signed document, so what they
/// check is what ships.
fn release_view(
    manifest: Option<&gxcore::Manifest>,
    source: &str,
    allow_network: bool,
    disk: &Disk<'_>,
) -> ReleaseStateDto {
    let identity = disk.identity;
    let installed_from = identity.from;
    let installed_commit = identity.commit.clone();
    let binary = identity.binary.clone().unwrap_or_default();
    // The record's ordinal dates the ENGINE only when the record describes the engine. A
    // launcher-only record's ordinal is the launcher's release, and the screen would print it
    // under "Released" beside an engine build it has nothing to do with.
    let installed_ordinal = match (installed_from, disk.record) {
        (InstalledFrom::Record, Some(r)) => r.ordinal,
        _ => 0,
    };
    let base = |kind: &str, detail: String| ReleaseStateDto {
        installs_off: gxio::release::refuse_install_on_test_profile().is_err(),
        verdict: kind.into(),
        headline: String::new(),
        source: source.into(),
        platform: disk.platform.into(),
        available_ordinal: 0,
        available_commit: String::new(),
        installed_ordinal,
        installed_commit: installed_commit.clone(),
        installed_from: installed_from.as_str().into(),
        installed_built: binary.built.clone(),
        recorded_commit: binary.recorded.clone(),
        installed_unpublished: binary.unpublished,
        joins: String::new(),
        has_record: disk.record.is_some(),
        items: Vec::new(),
        total_bytes: 0,
        total_estimated: false,
        install_dir: disk.install_dir.clone(),
        detail,
    };

    let Some(m) = manifest else {
        let (kind, detail) = unknown_kind(allow_network);
        return ReleaseStateDto {
            headline: headline(kind, identity, "").into(),
            joins: who_can_join(kind, &installed_commit, "", &[]),
            ..base(kind, detail)
        };
    };

    let available_commit = published_engine_commit(m, disk.platform);
    let verdict = outstanding(m, disk.record, disk.platform, disk.own, &disk.build_commit, disk.appimage, disk.engine.as_ref());
    let kind = verdict_kind(&verdict, installed_from, &installed_commit, &available_commit);
    // THE PAYLOAD BELONGS TO THE VERDICT THAT IS ACTUALLY REPORTED - empty for every kind but
    // `update`, so nothing is ever listed under words that say nothing will download. On Windows the
    // updater's own --check plan replaces the launcher's guess at it, so the estimate is the plan.
    let (kind, items, total, total_estimated) = reconcile_plan(kind, &verdict, disk);
    let detail = describe(kind, &verdict, &items, total, total_estimated, source, &available_commit, identity);

    // A RE-FETCH OF THE ENGINE AT AN IDENTICAL COMMIT IS NOT A NEW BUILD. When the installed engine
    // commit already equals the published one and the plan STILL fetches the engine tier, the install
    // could not be verified against its record and is being re-downloaded to restore it - never a
    // newer release. "Update available" there is the lie the owner photographed ("You have X / New X,
    // Update available"). A same-commit plan that touches only the launcher or the data tiers is a
    // genuine update and keeps its words - the macOS launcher-only update is exactly that.
    let reinstalling = kind == "update"
        && !installed_commit.is_empty()
        && installed_commit == available_commit
        && items.iter().any(|i| i.label == "engine");
    let headline_str: &str = if reinstalling {
        "Reinstalling this release"
    } else {
        headline(kind, identity, &available_commit)
    };

    ReleaseStateDto {
        headline: headline_str.into(),
        available_ordinal: m.ordinal,
        joins: who_can_join(kind, &installed_commit, &available_commit, &m.artifacts),
        items,
        total_bytes: total,
        total_estimated,
        available_commit,
        ..base(kind, detail)
    }
}

/// Reconcile the launcher's own `decide` with the updater's authoritative `--check` plan.
///
/// On Windows `disk.win_plan` is what `GeneralsXUpdate.exe` will actually download - the same
/// `state_load` + `plan_update` the press runs. It replaces the launcher's guess entirely: a
/// non-empty plan IS an update (with the updater's tiers, and the updater's total when it is
/// priced), and an empty plan is nothing to fetch, so an "update" `decide` reached from a record
/// the updater has since discarded is downgraded rather than shown with a wrong figure. A
/// non-empty plan whose total is 0 is not priced - see the "unpriced" comment below - and is
/// flagged estimated rather than shown as a 0-byte download. Only when the updater cannot be asked
/// does `decide`'s own payload stand - exact where the launcher IS the updater (macOS, Linux), an
/// ESTIMATE on Windows, which the page then labels as one.
fn reconcile_plan(
    kind: &'static str,
    verdict: &gxcore::Verdict,
    disk: &Disk<'_>,
) -> (&'static str, Vec<ReleaseItemDto>, u64, bool) {
    if let Some(plan) = disk.win_plan.as_ref() {
        if plan.rows.is_empty() {
            // The updater will fetch nothing. An "update" derived from a stale record is not real.
            return (if kind == "update" { "uptodate" } else { kind }, Vec::new(), 0, false);
        }
        let mut items: Vec<ReleaseItemDto> = plan
            .rows
            .iter()
            .map(|r| ReleaseItemDto {
                platform: r.platform.clone(),
                name: r.name.clone(),
                size: r.size,
                label: tier_label(&r.name),
            })
            .collect();
        // Biggest first, matching how the download is actually felt - the same order `payload` uses.
        items.sort_by(|a, b| b.size.cmp(&a.size));
        // NOT `plan.total == 0` (Codex, second round): `plan_update` sums only the rows it CAN
        // price, so a plan with one unpriced row and one priced one carries a nonzero partial
        // total - `print_plan` forces the printed total to 0 only when EVERY row is unpriced. A
        // partial sum shown as the whole is the same lie the all-zero case is, just smaller, so
        // the signal has to be "does any pending row still say size 0", never the total.
        let unpriced = plan.rows.iter().any(|r| r.size == 0);
        return ("update", items, plan.total, unpriced);
    }
    let (items, total) = payload(verdict, kind);
    // The launcher had to guess. That guess is exact off Windows; on Windows it is the one that can
    // name a single tier as the whole, so the page says the real total is still being worked out.
    let estimated = kind == "update" && disk.platform == "win32";
    (kind, items, total, estimated)
}

/// THE ENGINE COMMIT THE PUBLISHED RELEASE CARRIES FOR THIS PLATFORM - what an install's engine
/// commit (BUILD.txt, the launcher's vouched install, the engine binary) is compared with.
///
/// From this platform's ENGINE row name, never the signed `sha` field first (seventh Codex round,
/// 18/09/2026). gx-manifest.py signs the commit it RUNS at, and publish-bootstrap.sh (:376-403)
/// lets a launcher-only release move that commit without rebuilding any engine - so after one,
/// the signed commit is the launcher's, and every install of the unchanged engine read "Not the
/// published release". The row names are signed too, and carry the engine's commit by the same
/// last-dash rule the updater's `state_load` reads.
///
/// EMPTY when the document names no engine for the platform - never the signed commit in its place
/// (tenth Codex round). That commit is whatever HEAD was when the document was generated: the
/// engine's for the live document, but the launcher row's for every document up to 11/09/2026
/// (self_update.rs, `plan_self_update`) and for a launcher-only publish. [`verdict_kind`] then
/// has no engine to compare with.
fn published_engine_commit(m: &gxcore::Manifest, platform: &str) -> String {
    m.artifacts
        .iter()
        .filter(|a| a.platform.eq_ignore_ascii_case(platform) && !is_launcher_artifact(&a.name))
        .find_map(|a| commit_in_artifact_name(&a.name))
        .unwrap_or_default()
}

/// The card's headline - the words beside the status dot on the home screen and the update screen.
///
/// HERE, NOT IN THE PAGES (fifth review, 18/09/2026). MainWeb and MainUpdate each carried a copy of
/// this choice in JavaScript, and a test could not reach either; the truth table below holds this
/// one, and both pages paint it. Every phrase is as strong as its evidence, as `unmanaged_detail`
/// explains.
fn headline(kind: &str, identity: &Identity, available_commit: &str) -> &'static str {
    match kind {
        "uptodate" => "Up to date",
        "update" => "Update available",
        "older" => "Ahead of what is published",
        "unknown" => "Could not check",
        "unmanaged" => match identity.from {
            // A packaged BUILD.txt or the launcher's own install names the build: a match is
            // `uptodate`, so here a different release is published.
            InstalledFrom::Build | InstalledFrom::Installer => "Not the published release",
            InstalledFrom::Binary => {
                let b = identity.binary.as_ref();
                if !identity.commit.is_empty() && identity.commit == available_commit {
                    "Built from the release commit"
                } else if b.and_then(|b| b.unpublished) == Some(true) {
                    "Development build"
                } else {
                    "Not the published release"
                }
            }
            InstalledFrom::Record | InstalledFrom::Nothing => "Not from a release",
        },
        _ => "",
    }
}

/// WHAT WOULD DOWNLOAD - THE ONE RULE every update screen reads (fifth review, 18/09/2026).
///
/// It answers for the thing that would actually do the downloading, per platform, because the
/// three are different programs with different evidence:
///
/// * WINDOWS - `GeneralsXUpdate.exe`, which applies EVERY row `decide` lists against its own record
///   (gxlaunch.c:2168-2181): the engine, the runtime tier the launcher rides in, the data tiers.
///   Since fc33f7efd (26/08/2026) this ran the macOS/Linux filter on Windows too - which keeps only
///   `GeneralsXLauncher-*` rows, and the Windows document has none - so every Windows row was
///   removed and a Windows install four rows behind read "Up to date, nothing to download".
///   Mirrored in the updater's own order and nothing more: a record that `BUILD.txt` contradicts is
///   discarded first, ordinal and all, as `state_load` discards it; what is left - or nothing -
///   goes to `decide`, which is `plan_update`. No record therefore offers every row, even beside a
///   `BUILD.txt` at the published commit: that file rides the engine tier and vouches for nothing
///   else, every bundle has shipped its record since 19/08 so a missing one was deleted (the
///   record's header: "delete to force a re-download"), and Update is the only way this launcher
///   runs the updater. Calling that "Up to date" hid the one repair there is.
/// * macOS AND LINUX - this launcher, which can apply its own row - and on macOS the ENGINE row too
///   (`engine`, 28/09/2026: the verdict used to name the launcher alone there, and a newer engine was
///   installed unasked, so MainWeb could not hand the Mac over to MainUpdate as it does Windows).
///   Whether the launcher row is outstanding is [`launcher_outstanding`], judged against the RUNNING BINARY. It used to be
///   two rules - a hash comparison against a launcher-only record, `plan_self_update` without one
///   - and a stale record beside a development launcher, or one newer than published, read "Update
///   available" while the background update refused to install it. The record keeps one job here:
///   its ordinal above the document's is `older`, `decide`'s floor, which `release_apply` applies
///   too.
fn outstanding(
    m: &gxcore::Manifest,
    record: Option<&gxcore::InstallState>,
    platform: &str,
    own: crate::self_update::OwnBuild<'_>,
    build_commit: &str,
    appimage: bool,
    engine: Option<&gxcore::Artifact>,
) -> gxcore::Verdict {
    if platform == "win32" {
        let kept = record.filter(|st| !record_contradicts_build(st, build_commit)).cloned();
        return gxcore::decide(m, &kept.unwrap_or_default(), platform);
    }
    if let Some(st) = record.filter(|st| m.ordinal < st.ordinal) {
        return gxcore::Verdict::OlderThanInstalled { manifest: m.ordinal, installed: st.ordinal };
    }
    let rows: Vec<gxcore::Artifact> =
        launcher_outstanding(m, platform, own, record, appimage).into_iter().chain(engine.cloned()).collect();
    if rows.is_empty() { gxcore::Verdict::UpToDate } else { gxcore::Verdict::Update(rows) }
}

/// Does `BUILD.txt` contradict the record, so `GeneralsXUpdate.exe` discards it? The updater's own
/// rule (gxlaunch.c `state_load`): among record rows whose name ends in a 7+ character token after
/// its last dash, not one starts with the first 7 characters of `BUILD.txt`'s commit. No
/// `BUILD.txt`, or no checkable row, never contradicts. It means a setup was run over the install:
/// the FILES are what `BUILD.txt` names and the record is stale - so it is neither evidence of the
/// engine ([`identify`]) nor a basis for what would download ([`outstanding`]).
fn record_contradicts_build(st: &gxcore::InstallState, build_commit: &str) -> bool {
    let Some(want) = build_commit.get(..7) else { return false };
    let tails: Vec<&str> = st
        .items
        .iter()
        .filter_map(|i| i.name.rsplit_once('-').map(|(_, t)| t))
        .filter(|t| t.len() >= 7)
        .collect();
    !tails.is_empty() && !tails.iter().any(|t| t.starts_with(want))
}

/// This platform's published launcher row, IF this launcher should replace itself with it.
///
/// THE ONE RULE, judged against the RUNNING BINARY - never the install record, which says what was
/// installed once. The home screen, the update panel, the pressed Update (`release_apply`) and the
/// background update all follow it:
///
/// * a binary WITH a commit time (every packaged macOS launcher) - exactly `plan_self_update`, the
///   rule the background update runs: a different commit, published strictly later than this one
///   was committed;
/// * no commit time on macOS - a development build, which never replaces itself;
/// * no commit time on Linux - EVERY shipped AppImage (remote-build-linux.sh sets GX_COMMIT and no
///   GX_COMMIT_EPOCH, and a test holds that). The pressed Update is the only way a Linux launcher
///   updates, so a published AppImage at any other commit is outstanding - and so is one at THIS
///   commit whose bytes the record of the last pressed Update says differ (a republish with a
///   corrected hash, which `decide` treats as an update too). ponytail: no direction guard on
///   Linux - an AppImage NEWER than the published one is offered the published one; giving the
///   Linux build a commit time (and its test a new rule) is the upgrade.
///
/// macOS never offers a same-commit republish, exactly as its background update never installs
/// one (`plan_self_update`'s `Current`): there is no pressed Update there to deliver it.
///
/// And only for a launcher running FROM an AppImage (`appimage`, `locate_target`'s answer): a
/// `cargo run` or hand-built Linux launcher has no commit time either, and offering it the AppImage
/// sent it to MainUpdate, whose Update can only fail - away from PLAY (eleventh Codex round).
pub(crate) fn launcher_outstanding(
    m: &gxcore::Manifest,
    platform: &str,
    own: crate::self_update::OwnBuild<'_>,
    record: Option<&gxcore::InstallState>,
    appimage: bool,
) -> Option<gxcore::Artifact> {
    use crate::self_update::SelfUpdatePlan;
    match crate::self_update::plan_self_update(m, platform, own) {
        SelfUpdatePlan::Install(row) => Some(row),
        SelfUpdatePlan::Current(_) => None,
        // `applicable_rows`, the panel's own count, so the display and the swap share one selector.
        SelfUpdatePlan::Nothing(_) if platform == "linux" && own.commit_epoch.is_none() && appimage => {
            applicable_rows(m.artifacts.clone(), platform)
                .into_iter()
                .next()
                .filter(|row| {
                    commit_in_artifact_name(&row.name).as_deref() != Some(own.commit9)
                        || record
                            .and_then(|st| st.has(&row.platform, &row.name))
                            .is_some_and(|have| have.hash != row.hash)
                })
        }
        SelfUpdatePlan::Nothing(_) => None,
    }
}

/// What will download, for the verdict ACTUALLY reported - empty for every kind but `update`.
fn payload(verdict: &gxcore::Verdict, kind: &str) -> (Vec<ReleaseItemDto>, u64) {
    match (verdict, kind) {
        (gxcore::Verdict::Update(arts), "update") => {
            let total = arts.iter().map(|a| a.size).sum();
            let mut v: Vec<ReleaseItemDto> = arts
                .iter()
                .map(|a| ReleaseItemDto {
                    platform: a.platform.clone(),
                    name: a.name.clone(),
                    size: a.size,
                    label: tier_label(&a.name),
                })
                .collect();
            // Biggest first, matching how the download is actually felt.
            v.sort_by(|a, b| b.size.cmp(&a.size));
            (v, total)
        }
        _ => (Vec::new(), 0),
    }
}

/// What identifies this install, and therefore how much a verdict about it can know.
///
/// NOT THE SAME QUESTION AS "where did the commit string come from". A record whose artifact names
/// are unreadable still carries an ordinal and a hash per artifact, so it is still `Record`; what
/// this ranks is the EVIDENCE the verdict rests on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum InstalledFrom {
    /// `gxinstalled.txt`, on Windows, where GeneralsXUpdate.exe writes it for every tier. An
    /// ordinal and a hash per artifact - everything `gxcore::decide` wants. See [`identify`].
    Record,
    /// `BUILD.txt` alone. A commit, and NO ordinal. A fresh extract of the Windows share zip is
    /// exactly this until the updater first runs, because the packager ships `BUILD.txt` inside
    /// the payload and only `GeneralsXUpdate.exe` ever writes the record.
    Build,
    /// Neither file, but the engine binary carries its own gitinfo stamp. A development copy that
    /// can still say which commit it is.
    Binary,
    /// The engine binary names a commit AND the launcher's own install vouches for that exact
    /// binary (`gxio::engine::launcher_vouches_for`: its record, the stamp it wrote inside the
    /// bundle, and the executable's hash all agree). A macOS install made by this launcher - which
    /// ships no `BUILD.txt` - and the one binary-sourced state that can be the release.
    Installer,
    /// None of the three. Nothing on this disk says which build this is.
    Nothing,
}

impl InstalledFrom {
    /// The serialized token. Lowercase and stable, because a screen branches on it.
    fn as_str(self) -> &'static str {
        match self {
            InstalledFrom::Record => "record",
            InstalledFrom::Build => "build",
            InstalledFrom::Binary => "binary",
            InstalledFrom::Installer => "installer",
            InstalledFrom::Nothing => "none",
        }
    }
}

/// What the engine binary told us, for the one case where nothing better did.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct BinaryEvidence {
    /// RFC 3339 commit date, or empty.
    built: String,
    /// The launcher's engine record, short form - set ONLY when it disagrees with the binary.
    recorded: String,
    /// `Some(true)` when no published release carries this commit; `None` unless a changelog
    /// fetched in this check could say (see [`release_list_for_absence`]).
    unpublished: Option<bool>,
}

/// What this install is running, on what evidence, and what the binary adds when it had to answer.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Identity {
    commit: String,
    from: InstalledFrom,
    binary: Option<BinaryEvidence>,
}

/// What this install is running, and on what evidence. THE DISK HALF, and the ONLY one: the home
/// screen (`build_release_state`) and the Changelogs screen (`build_changelog`) both call this, so
/// the two cannot describe one install two ways.
///
/// `record` is the parsed `gxinstalled.txt`, or `None` when there is none - the caller's own read,
/// passed in rather than re-read. `releases` is asked for only if the engine binary answers; see
/// [`release_list_for_absence`].
///
/// THE ENGINE BINARY IS ASKED LAST, AND ONLY WHEN NOTHING BETTER ANSWERED. A release install is
/// therefore answered exactly as it was before the binary was a source - same file, same
/// precedence, same verdict - and the 16 MB read happens only on the copies that used to print
/// dashes.
///
/// THE LAUNCHER'S ENGINE RECORD IS READ THROUGH THE CHECKED DOOR. `installed_sha()` is the raw
/// record, and its own doc says display code must use `installed_sha_if_still_installed()`: the
/// raw one survives the bundle it names being deleted or moved, and would then have this screen say
/// the launcher installed a build "here" that is not here at all.
fn installed_identity(
    record: Option<&gxcore::InstallState>,
    releases: impl FnOnce() -> Option<Vec<String>>,
) -> Identity {
    identify(
        rel::platform_token(),
        record,
        rel::engine_build_stamp()
            .map(|s| rel::short_commit(&s.commit))
            .unwrap_or_default(),
        gxio::enginestamp::find_engine_stamp,
        gxio::engine::launcher_vouches_for,
        gxio::engine::installed_sha_if_still_installed,
        releases,
    )
}

/// The pure rule behind [`installed_identity`]: every source supplied, the costly ones as closures
/// so that "asked last" is something a test can hold rather than a comment.
///
/// `build_commit` is `BUILD.txt`'s commit, short form, or empty.
///
/// THE UPDATE RECORD NAMES THE ENGINE ON WINDOWS ONLY (fourth Codex round, 18/09/2026). There
/// GeneralsXUpdate.exe writes it for every tier it applies; an unreadable one parses to no rows and
/// stays `Record`, because `decide` then plans every row, which is what the updater will do. On
/// macOS and Linux the one writer is this launcher's own update, which records its own row and
/// nothing else, so the file is never evidence of the ENGINE: not a launcher row, not an empty or
/// unreadable file, not engine rows somebody copied in. Since the launcher row is judged against
/// the running binary, an empty macOS record counted as `Record` read "Up to date" over any engine.
/// A record `BUILD.txt` contradicts is discarded as `state_load` discards it.
fn identify(
    platform: &str,
    record: Option<&gxcore::InstallState>,
    build_commit: String,
    stamp: impl FnOnce() -> Option<(std::path::PathBuf, gxio::enginestamp::EngineStamp)>,
    vouch: impl FnOnce(&std::path::Path) -> Option<String>,
    recorded: impl FnOnce() -> Option<String>,
    releases: impl FnOnce() -> Option<Vec<String>>,
) -> Identity {
    let record = record.filter(|_| platform == "win32");
    let engine_record = record.is_some_and(|st| !record_contradicts_build(st, &build_commit));
    // `BUILD.txt` first: it ships inside the payload, so it is written by the same extraction that
    // puts the engine on disk. The record's artifact names are the fallback, launcher rows skipped.
    let commit = if !build_commit.is_empty() {
        build_commit
    } else {
        record.and_then(commit_from_record).unwrap_or_default()
    };
    if engine_record || !commit.is_empty() {
        return identity_from(engine_record, commit, None, None, None);
    }
    let Some((bin, s)) = stamp() else {
        return identity_from(false, commit, None, None, None);
    };
    // THE LAUNCHER'S OWN INSTALL, WHEN IT STILL VOUCHES FOR THIS BINARY. Its record used to be read
    // only to report a DISAGREEMENT; agreeing, with the stamp it wrote in the bundle and the
    // executable's hash, it is the proof a macOS release install has in place of `BUILD.txt`.
    let short = rel::short_commit(&s.commit);
    if vouch(&bin).is_some_and(|v| rel::short_commit(&v) == short) {
        return Identity {
            commit: short,
            from: InstalledFrom::Installer,
            binary: Some(BinaryEvidence { built: s.built, recorded: String::new(), unpublished: None }),
        };
    }
    let recorded = recorded();
    let releases = releases();
    identity_from(false, commit, Some(s), recorded, releases.as_deref())
}

/// Is this artifact the LAUNCHER, rather than the engine or a data tier?
fn is_launcher_artifact(name: &str) -> bool {
    name.starts_with("GeneralsXLauncher-")
}

/// The engine commit a record's artifact names carry, if any, using the rule `state_load` compares
/// against `BUILD.txt` with - the text after the LAST dash (gxlaunch.c:1609-1627).
///
/// LAUNCHER ROWS ARE SKIPPED. `GeneralsXLauncher-macos-fc33f7efd.tar.gz` carries a commit too - the
/// LAUNCHER's - and taking the first commit-bearing row put that on the home screen as the build of
/// the game.
fn commit_from_record(st: &gxcore::InstallState) -> Option<String> {
    st.items
        .iter()
        .filter(|it| !is_launcher_artifact(&it.name))
        .find_map(|it| commit_in_artifact_name(&it.name))
}

/// How long a changelog THIS PROCESS fetched may stand for "now". Every screen that shows a verdict
/// calls `changelog_check` beside it, so in use the list is seconds old; the bound only stops a
/// launcher left open for a day from judging a release published since.
const FETCHED_FRESH_FOR: std::time::Duration = std::time::Duration::from_secs(10 * 60);

/// The commits of the last changelog `build_changelog` FETCHED in this process, and when.
static FETCHED_RELEASES: std::sync::Mutex<Option<(std::time::Instant, Vec<String>)>> =
    std::sync::Mutex::new(None);

/// The last fetched list and its age, or `None` if this process never fetched one.
fn fetched_releases() -> Option<(std::time::Duration, Vec<String>)> {
    let g = FETCHED_RELEASES.lock().ok()?;
    g.as_ref().map(|(at, list)| (at.elapsed(), list.clone()))
}

/// The commits a changelog publishes, short form - but ONLY when that changelog can prove an
/// ABSENCE, which is what "Development build" claims ("no published release carries this").
///
/// * the cached FILE - never. It is as old as the last fetch, and a release published since is
///   simply missing from it, so its silence is not evidence. This used to read it and nothing
///   else, and could call a genuine release a development build.
/// * a list this process fetched more than [`FETCHED_FRESH_FOR`] ago - `None`, for that reason.
/// * a list at `CHANGELOG_MAX_RELEASES` - `None`. Both the generator and the parser stop there, so
///   a release older than the 200th is dropped and an absence proves nothing.
///
/// NEVER A FETCH OF ITS OWN (18/09/2026, second review). The first version fetched here, serially,
/// before the manifest: with the CDN down `release_check` waited out curl's 8 s before even asking
/// for the manifest, and repeated the request `changelog_check` was already making. The cost of not
/// fetching: a check that reads the list before `changelog_check` has fetched it says the weaker,
/// true "Not the published release" - so MainWeb asks once more after both have settled, for that
/// state only (`mainweb_asks_again_once_the_changelog_has_landed`).
fn release_list_for_absence(fetched: Option<(std::time::Duration, Vec<String>)>) -> Option<Vec<String>> {
    let (age, list) = fetched?;
    if age > FETCHED_FRESH_FOR || list.is_empty() || list.len() >= rel::CHANGELOG_MAX_RELEASES {
        return None;
    }
    Some(list)
}

/// THE PURE HALF of [`installed_identity`]: every input supplied, so the combinations are tested
/// rather than asserted about. It reads no disk.
///
/// `releases` is every commit the changelog publishes, short form, or `None` when there was no
/// changelog - and `None` is kept distinct from an empty list on purpose, because "no release
/// carries this" is a claim only a document that was actually read can make.
fn identity_from(
    has_record: bool,
    commit: String,
    stamp: Option<gxio::enginestamp::EngineStamp>,
    recorded: Option<String>,
    releases: Option<&[String]>,
) -> Identity {
    if has_record {
        return Identity { commit, from: InstalledFrom::Record, binary: None };
    }
    if !commit.is_empty() {
        return Identity { commit, from: InstalledFrom::Build, binary: None };
    }
    let Some(s) = stamp else {
        return Identity { commit, from: InstalledFrom::Nothing, binary: None };
    };
    let short = rel::short_commit(&s.commit);
    // Only a DISAGREEMENT is worth reporting. A record that agrees with the binary adds nothing
    // the Build row does not already say.
    let recorded = recorded
        .map(|r| rel::short_commit(&r))
        .filter(|r| !r.is_empty() && *r != short)
        .unwrap_or_default();
    let unpublished = releases.map(|rs| !rs.iter().any(|r| *r == short));
    Identity {
        commit: short,
        from: InstalledFrom::Binary,
        binary: Some(BinaryEvidence { built: s.built, recorded, unpublished }),
    }
}

/// The no-document state.
///
/// SEPARATE FROM [`verdict_kind`] ON PURPOSE, and it takes no argument about the install: no
/// evidence of what is installed can turn "we obtained nothing" into a statement about currency.
/// Keeping the two functions apart is what makes that structural rather than a comment somebody
/// later edits out.
fn unknown_kind(allow_network: bool) -> (&'static str, String) {
    // Never "up to date". That phrase is a promise that nothing will download, and this branch
    // cannot make it.
    let detail = if allow_network {
        "Could not reach the update server, and no document is cached. Play works normally."
    } else {
        "No update document has been cached yet."
    };
    ("unknown", detail.to_string())
}

/// Which of the four document-in-hand states this is, once the evidence about the install is taken
/// into account.
///
/// PURE, so the combinations below are tested rather than asserted about. It reads no disk.
///
/// THE CORRECTION IT MAKES is for an install that no ENGINE record identifies. `BUILD.txt` (or the
/// launcher's own install, on macOS) names the build this install is and the manifest's SIGNED
/// `sha` field names the commit the published release was built from, so when those two agree,
/// this install IS that release. Two measured values compared is knowledge.
///
/// It does NOT invent a direction when they disagree. A build stamp carries no ordinal, so
/// `decide`'s own ordinal comparison stays the only thing that may say `older`; a disagreement is
/// `unmanaged` unless something this launcher can apply is really outstanding.
///
/// `verdict` is [`outstanding`]'s: real arithmetic whether or not a record exists, so every arm
/// may trust what it says is outstanding.
fn verdict_kind(
    verdict: &gxcore::Verdict,
    from: InstalledFrom,
    installed_commit: &str,
    available_commit: &str,
) -> &'static str {
    let decided = match verdict {
        gxcore::Verdict::UpToDate => "uptodate",
        gxcore::Verdict::OlderThanInstalled { .. } => "older",
        gxcore::Verdict::Update(_) => "update",
    };

    match from {
        InstalledFrom::Record => decided,
        // EVERY OTHER SOURCE, IN ONE ORDER (fourth review, 18/09/2026). `Build` used to pass
        // `decided` through on a commit mismatch, which was right while `decided` was the whole
        // manifest; it is the rows THIS launcher can apply, and on Linux an older engine beside a
        // current launcher came out "Up to date / Nothing will download". And it put a commit match
        // FIRST, hiding an outstanding AppImage - on Linux, where nothing else offers it.
        //
        //   1. Whatever `decided` names outstanding (the launcher) or calls older stands.
        //   2. Then a commit match is `uptodate` - but only from evidence that names the BUILD:
        //      `BUILD.txt`, or the launcher's own install vouching for the executable (`Installer`).
        //      The engine's bare gitinfo names a commit, which any build at it carries - with
        //      uncommitted changes, other flags, a 60 Hz logic rate. Measured on the owner's Mac:
        //      `GitUncommittedChanges` reads 1 on the release engine as well, and the ZH source digest
        //      is 0x9EF15386 in both, so nothing in the binary tells them apart.
        //   3. Anything else is `unmanaged`: never an "uptodate" that only says the launcher is
        //      current, and never "update" for rows this launcher cannot apply.
        _ => {
            if decided != "uptodate" {
                decided
            } else if matches!(from, InstalledFrom::Build | InstalledFrom::Installer)
                && !installed_commit.is_empty()
                // A document that names no engine for this platform publishes nothing this build
                // could be behind: evidence that names the build, and nothing outstanding, is all
                // "Up to date" can rest on there (tenth Codex round). The binary never gets here.
                && (installed_commit == available_commit || available_commit.is_empty())
            {
                "uptodate"
            } else {
                "unmanaged"
            }
        }
    }
}

/// `GeneralsX-win64-57d14a20b.zip` -> `57d14a20b`.
///
/// The commit is the LAST dash-separated token and the extension is cut off it. Anything that is
/// not 7-40 hex characters is not a commit and yields nothing, because a tier file like
/// `gxrt_66dd5434.zip` has no dash at all and must not contribute a wrong answer.
pub(crate) fn commit_in_artifact_name(name: &str) -> Option<String> {
    let tail = name.rsplit_once('-')?.1;
    let tail = tail.split('.').next()?;
    if (7..=40).contains(&tail.len()) && tail.bytes().all(|b| b.is_ascii_hexdigit()) {
        Some(rel::short_commit(tail))
    } else {
        None
    }
}

/// A human name for an artifact, from the naming rules `package-windows-share.ps1` commits to.
/// Ported from `UpdateStatus.TierLabel` so both launchers say the same words about the same file.
///
/// FALLS BACK TO THE FILENAME, never to a guess. A tier added later shows up as its own name and
/// its real size, which is accurate and merely less pretty.
fn tier_label(name: &str) -> String {
    let lower = name.to_ascii_lowercase();
    if lower.starts_with("gxrt_") {
        "runtime (launcher and DLLs)".into()
    } else if lower.starts_with("gxreq_") {
        "required game data".into()
    } else if lower.starts_with("gxmdl_") {
        // the 3D model pack's own row (package-windows-share.ps1 $ownArtifact: alone it stays under the updaters' 512 MiB cap)
        "required game data (3D models)".into()
    } else if lower.starts_with("gxopt_") {
        "optional extras".into()
    } else if lower.starts_with("generalsxlauncher-") {
        // The launcher's own row. Its name carries a platform token too, and the rule below would
        // call it the engine - which is what the home screen said about the launcher's own update.
        "launcher".into()
    } else if lower.contains("-win64-") || lower.contains("-macos-") || lower.contains("-linux-") {
        "engine".into()
    } else {
        name.to_string()
    }
}

/// One sentence, chosen by state. Written here so the phrasing lives in one place rather than in
/// four screens that will drift.
fn describe(
    kind: &str,
    verdict: &gxcore::Verdict,
    items: &[ReleaseItemDto],
    total: u64,
    total_estimated: bool,
    source: &str,
    published: &str,
    identity: &Identity,
) -> String {
    let seen = match source {
        "network" => "the update server",
        "relay" => "a relay",
        _ => "the last document seen",
    };
    match kind {
        "uptodate" => {
            if source == "cache" {
                // The cache agrees with the install BY CONSTRUCTION, so it can never be the
                // basis for "up to date" - that is the whole bug UpdateStatus.cs documents.
                "Nothing was outstanding in the last document seen. Not yet re-checked.".into()
            } else {
                format!("Checked against {seen}. Nothing will download.")
            }
        }
        // THE TIERS ARE NAMED, NOT JUST TOTALLED, and that is the difference between an honest
        // line and a confusing one. The payload is split into four tiers and the launcher itself
        // rides the runtime tier, so a release can leave the ENGINE COMMIT UNTOUCHED while other
        // tiers move - which puts the same nine characters under both "You have" and "New" beside
        // a real download. Observed with the live document. Naming what would be fetched is what
        // makes that read as a fact rather than a contradiction. Same reasoning, and the same
        // words, as UpdateStatus.DescribePlan.
        "update" => {
            // THE SAME UNPRICED STATE `reconcile_plan` FLAGS ON THE TOTAL reaches this sentence
            // too (Codex, second round) - the total used to fall back to a raw `mb(total)` here
            // regardless, so an unpriced plan read "0.0 MB to download ... engine 0.0 MB": the
            // exact lie `total_estimated` exists to stop, just in the sentence rather than the
            // numeric row. A row's own 0 only means "not yet known" when the WHOLE plan is
            // unpriced - `payload`'s guessed items carry real manifest sizes and never mean this.
            let parts: Vec<String> = items
                .iter()
                .map(|i| {
                    let size = if total_estimated && i.size == 0 {
                        "size not yet known".to_string()
                    } else {
                        mb(i.size)
                    };
                    format!("{} {size}", i.label)
                })
                .collect();
            let total_str = if total_estimated { "still being worked out".to_string() } else { mb(total) };
            format!("{total_str} to download from {}: {}", seen, parts.join(", "))
        }
        "older" => match verdict {
            gxcore::Verdict::OlderThanInstalled { manifest, installed } => format!(
                "The published document (#{manifest}) is older than this install (#{installed}). \
                 Nothing will download."
            ),
            _ => "Nothing will download.".into(),
        },
        // NOT A WARNING, because there is nothing here for a player to act on. This state means
        // neither a build stamp nor an update record is on disk, which is what a build run from a
        // development tree looks like - and the sentence that used to sit here ("this install
        // carries no update record, so the launcher cannot say what it already has") read as a
        // fault report about the player's install, in yellow, permanently, on every macOS and
        // Linux launch. A shipped install has a BUILD.txt and no longer reaches this arm at all.
        "unmanaged" => unmanaged_detail(&short_or(published), identity),
        _ => String::new(),
    }
}

/// The one sentence for a copy no release record identifies. PURE, so every combination is tested.
///
/// EACH CLAIM IS ONLY AS STRONG AS ITS EVIDENCE, which is why there are four phrasings of the
/// same situation rather than one:
///
/// * The published release's own commit is NOT called the release. The engine's gitinfo names a
///   commit, not a build - see `verdict_kind` - so the sentence says the commit matches and that
///   nothing here shows the build does.
/// * "Development build" only when a freshly fetched changelog was READ and no release in it
///   carries this commit. That is a measured absence. Without one the commit could be an old
///   release, so the sentence says only what is certain - that it is not the one published now.
/// * A commit another release carries is named as that commit, never as "release X": the same
///   reason as the first point.
/// * The launcher's own record is named only when it DISAGREES with the binary, and it is named as
///   what it is: a record of what was installed once, now contradicted by what is on disk.
/// * With no stamp at all the sentence says why nothing is shown, in one line, and no longer calls
///   that "normal for a development tree" - a development build carries a stamp and lands in the
///   arm above, so reaching this one now means something unusual.
fn unmanaged_detail(published: &str, identity: &Identity) -> String {
    // ONE SENTENCE, ONE FULL STOP, IN EVERY ARM. That is the house voice for `detail` and an
    // existing test holds it by counting '.' characters - which is why no filename with a dot in
    // it (`BUILD.txt`) is written into any of these, and why the record clause is joined with a
    // semicolon rather than started as a second sentence.
    if identity.from == InstalledFrom::Build {
        // The packager's stamp names this build; it is simply not the one published now.
        return format!("This install is build {}, not the published release {published}.", identity.commit);
    }
    let Some(b) = identity.binary.as_ref() else {
        return format!(
            "Release {published} is published, but nothing on this disk names this build: no update \
             record, no packaged build stamp, and no stamp readable in the engine."
        );
    };
    let c = &identity.commit;
    let head = if identity.from == InstalledFrom::Installer {
        // The launcher installed this release and the binary is still the one it installed.
        format!("This install is release {c}, not the published release {published}")
    } else if !c.is_empty() && c == published {
        format!(
            "Built from {c}, the published release's commit, but nothing on this disk shows it was \
             built the way that release was"
        )
    } else {
        match b.unpublished {
            Some(true) => format!("Development build {c}, not the published release {published}"),
            Some(false) => {
                format!("Built from {c}, the commit of another release, not the published release {published}")
            }
            None => format!("Built from {c}, which is not the published release {published}"),
        }
    };
    if b.recorded.is_empty() {
        format!("{head}.")
    } else {
        format!(
            "{head}; the launcher recorded installing {} here, so this engine was replaced outside it.",
            b.recorded
        )
    }
}

/// The "Players can join" row: platforms for the published release, the engine's own rule for
/// anything else, or nothing at all. PURE, so every state is tested.
///
/// WHAT ACTUALLY DECIDES A JOIN is not the platform. The engine refuses any peer whose tier-1 SimID
/// differs - engine, source, data and ordinal ids (SimulationId.h); the platform id is a tier-2
/// WARNING only. So the row can say one of exactly two backed things:
///
/// * THIS INSTALL IS THE PUBLISHED RELEASE (`uptodate`, and its commit is the one the SIGNED
///   manifest names): the platforms that release was published for, read off the manifest's engine
///   rows at that commit. Every one of those was built from the same tree for the same release.
/// * ANYTHING ELSE WITH A KNOWN BUILD: "matching builds only" - the engine's own rule, which holds
///   for every build and claims nothing about who matches.
///
/// And nothing when the build itself is unknown: no row beats a guessed one.
///
/// "SAME BUILD ONLY" IS DELIBERATELY NOT SAID. Measured 18/09/2026 on the owner's Mac: the
/// development build 3fd38c2da and the published release 8ff29efa7 have the SAME source digest
/// (ZH 0x9EF15386 at both commits, computed with the build's own simsourcedigest_watcher.cmake) -
/// the commits between them touched nothing the simulation digest covers. Whether they can play
/// then turns on the engine, data and ordinal ids, which the launcher cannot know for anyone else's
/// build. "Same build only" would have been a confident wrong answer; "matching builds" is not.
fn who_can_join(
    kind: &str,
    installed_commit: &str,
    available_commit: &str,
    artifacts: &[gxcore::Artifact],
) -> String {
    if installed_commit.is_empty() {
        return String::new();
    }
    if kind == "uptodate" && !available_commit.is_empty() && installed_commit == available_commit {
        // Engine rows at THIS commit only. The launcher rows (GeneralsXLauncher-*) are built from
        // their own commit and say nothing about who can play; tier files carry no commit at all.
        const ORDER: [(&str, &str); 4] =
            [("win32", "Windows"), ("macos", "macOS"), ("linux", "Linux"), ("ios", "iPadOS")];
        let names: Vec<&str> = ORDER
            .iter()
            .filter(|(token, _)| {
                artifacts.iter().any(|a| {
                    a.platform.eq_ignore_ascii_case(token)
                        && !a.name.starts_with("GeneralsXLauncher-")
                        && commit_in_artifact_name(&a.name).as_deref() == Some(available_commit)
                })
            })
            .map(|(_, name)| *name)
            .collect();
        if !names.is_empty() {
            return names.join(", ");
        }
        // Current, but the document lists no engine for any platform we can name: fall through to
        // the rule rather than print an empty list.
    }
    "matching builds only".to_string()
}

fn short_or(full: &str) -> String {
    if full.is_empty() {
        "(unknown)".into()
    } else {
        rel::short_commit(full)
    }
}

/// MB to one decimal, matching the byte counter the download itself shows.
fn mb(bytes: u64) -> String {
    format!("{:.1} MB", bytes as f64 / 1_048_576.0)
}

// ---------------------------------------------------------------------------------------------
// The changelog
// ---------------------------------------------------------------------------------------------

/// Which entry, if any, is the one this install is running - AT MOST ONE.
///
/// AN ORDINAL IDENTIFIES A RELEASE; A COMMIT DOES NOT. The live document published 20/08/2026
/// carries `258405c20` twice and `53b8e04e3` twice - a re-publish at a new ordinal ships the same
/// engine commit - so matching on the commit alone marks two entries INSTALLED and the reader has
/// no way to tell which one they have. So:
///
/// * with an update record, the ordinal decides and the commit is not consulted at all;
/// * without one, the commit is the only measured thing left, and only the NEWEST entry carrying
///   it is marked (the list is newest-first, so that is the first match);
/// * with neither, NOTHING is marked. That is correct rather than unhelpful: nothing is known.
fn which_entry_is_installed(
    entries: &[rel::ChangeRelease],
    installed_ordinal: u64,
    installed_commit: &str,
) -> Option<usize> {
    if installed_ordinal != 0 {
        return entries.iter().position(|e| e.ordinal == installed_ordinal);
    }
    if installed_commit.is_empty() {
        return None;
    }
    entries
        .iter()
        .position(|e| rel::short_commit(&e.commit) == installed_commit)
}

fn build_changelog(allow_network: bool) -> ChangelogDto {
    let record = rel::read_install_record().map(|(_, t)| gxcore::InstallState::parse(&t));
    // THE SAME IDENTITY THE HOME SCREEN USES. `releases` is never asked for: this screen marks an
    // entry, it does not claim an absence, so it has no use for a changelog fetched on its behalf.
    let identity = installed_identity(record.as_ref(), || None);
    let record_ordinal = record.as_ref().map(|r| r.ordinal).unwrap_or(0);

    let mut source = "none";
    let mut entries: Vec<rel::ChangeRelease> = Vec::new();

    if allow_network {
        if let Some(text) = rel::fetch_https(rel::CHANGELOG_URL, rel::CHANGELOG_MAX_BYTES) {
            let parsed = rel::parse_changelog(&text);
            if !parsed.is_empty() {
                // A fetch that succeeds but does not PARSE is deliberately not cached:
                // overwriting a good cached copy with an error page would turn a transient CDN
                // fault into a permanently empty changelog. Changelog.cs:245-246.
                let _ = rel::write_cache_atomic(rel::CHANGELOG_CACHE, &text);
                // Remembered for the verdict's "Development build" - see `release_list_for_absence`.
                if let Ok(mut g) = FETCHED_RELEASES.lock() {
                    let commits = parsed.iter().map(|r| rel::short_commit(&r.commit)).collect();
                    *g = Some((std::time::Instant::now(), commits));
                }
                entries = parsed;
                source = "network";
            }
        }
    }

    if entries.is_empty() {
        if let Some((_, text)) = rel::read_cached(rel::CHANGELOG_CACHE, rel::CHANGELOG_MAX_BYTES) {
            let parsed = rel::parse_changelog(&text);
            if !parsed.is_empty() {
                entries = parsed;
                source = "cache";
            }
        }
    }

    changelog_dto(entries, source, &identity, record_ordinal)
}

/// THE PURE HALF of [`build_changelog`]: which entry is this install, from the one identity.
///
/// The record's ordinal decides only when the record identifies the ENGINE (`from == Record`). A
/// launcher-only record's ordinal is the release the LAUNCHER came from, and marking that entry
/// would say the game is a build it may not be. Otherwise the commit decides - from `BUILD.txt`,
/// or from the engine binary, which is exactly what the home screen shows as the build.
fn changelog_dto(
    entries: Vec<rel::ChangeRelease>,
    source: &str,
    identity: &Identity,
    record_ordinal: u64,
) -> ChangelogDto {
    let installed_ordinal = if identity.from == InstalledFrom::Record { record_ordinal } else { 0 };
    let installed_index = which_entry_is_installed(&entries, installed_ordinal, &identity.commit);
    // Only an ordinal names a release; a commit is SAME COMMIT - see `installed_exact`.
    let installed_exact = installed_index.is_some() && installed_ordinal != 0;

    let out = entries
        .into_iter()
        .enumerate()
        .map(|(i, e)| ChangelogEntryDto {
            installed: Some(i) == installed_index,
            ordinal: e.ordinal,
            commit: e.commit,
            date: e.date,
            title: e.title,
            notes: e.notes,
        })
        .collect();

    ChangelogDto {
        source: source.into(),
        entries: out,
        installed_ordinal,
        installed_commit: identity.commit.clone(),
        installed_from: identity.from.as_str().into(),
        installed_exact,
    }
}

// ---------------------------------------------------------------------------------------------

#[cfg(test)]
mod applicable_tests {
    use super::applicable_here;

    #[test]
    fn an_engine_payload_is_not_something_the_updater_can_apply() {
        // The regression, pinned. Publishing engine rows so the setup page could fetch an engine
        // made `decide` return them here too, and nothing in this binary can install one - so the
        // verdict stayed "update available" after a successful self-update, permanently.
        assert!(!applicable_here("GeneralsXZH-macos-arm64-55739fcf9.zip", "macos"));
        assert!(!applicable_here("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz", "linux"));
        // ...and the linux engine is a .tar.gz, which is why the NAME is checked and not only the
        // suffix. A suffix-only rule would call this a macOS launcher.
        assert!(!applicable_here("GeneralsXZH-linux-x86_64-55739fcf9.tar.gz", "macos"));
    }

    #[test]
    fn the_launcher_for_this_platform_still_is() {
        assert!(applicable_here("GeneralsXLauncher-macos-b0525f8cc.tar.gz", "macos"));
        assert!(applicable_here("GeneralsXLauncher-linux-x86_64-b0525f8cc.AppImage", "linux"));
    }

    #[test]
    fn the_other_platforms_launcher_is_not_ours_either() {
        assert!(!applicable_here("GeneralsXLauncher-linux-x86_64-b0525f8cc.AppImage", "macos"));
        assert!(!applicable_here("GeneralsXLauncher-macos-b0525f8cc.tar.gz", "linux"));
        // The community-patch overlay rides the "any" platform and is not a launcher.
        assert!(!applicable_here("gxreq_e2305004.zip", "macos"));
    }
}

#[cfg(test)]
mod apply_selector_tests {
    use super::{applicable_row, applicable_rows, select_launcher_artifact, target_suffix};
    use gxio::update::Target;

    fn row(platform: &str, name: &str) -> gxcore::Artifact {
        gxcore::Artifact {
            platform: platform.to_string(),
            name: name.to_string(),
            hash: "AAAAAAAAAAAAAAAAAAAAAA".to_string(),
            size: 1,
        }
    }

    fn mac() -> Target {
        Target::MacApp(std::path::PathBuf::from("/Applications/GeneralsX Launcher.app"))
    }

    fn appimage() -> Target {
        Target::AppImage(std::path::PathBuf::from("/home/p/GeneralsXLauncher.AppImage"))
    }

    /// THE FINDING. A manifest carrying an engine row AND a launcher row, engine FIRST, both with
    /// the suffix the macOS swap accepts.
    ///
    /// A selector that matches on the platform column and the file suffix alone takes the engine,
    /// because `find` takes the first match and the engine is first. What happens next is
    /// `apply_macos_tgz` unpacking an ENGINE payload over the running `.app` - the one irreversible
    /// step in the pipeline, with no rollback and nothing to run afterwards.
    ///
    /// Not reachable with today's published names (the macOS engine is a `.zip`), which is exactly
    /// why it needs a test rather than an observation: one repackaging makes it reachable, and the
    /// signature over the manifest does not help, because the payload is genuinely signed.
    #[test]
    fn an_engine_row_sharing_the_suffix_is_never_selected_over_the_launcher() {
        let wanted = vec![
            row("macos", "GeneralsXZH-macos-arm64-abc123456.tar.gz"),
            row("macos", "GeneralsXLauncher-macos-abc123456.tar.gz"),
        ];
        let got = select_launcher_artifact(&wanted, "macos", &mac())
            .expect("the launcher row is present and must be found");
        assert_eq!(
            got.name, "GeneralsXLauncher-macos-abc123456.tar.gz",
            "the engine payload was selected to be installed AS the launcher"
        );
    }

    /// The same manifest with the rows the other way round. Ordering must not be what saves it -
    /// a selector fixed by reordering the manifest is not fixed.
    #[test]
    fn the_launcher_is_selected_whichever_side_of_the_engine_it_is_published_on() {
        let wanted = vec![
            row("macos", "GeneralsXLauncher-macos-abc123456.tar.gz"),
            row("macos", "GeneralsXZH-macos-arm64-abc123456.tar.gz"),
        ];
        let got = select_launcher_artifact(&wanted, "macos", &mac()).expect("launcher row present");
        assert_eq!(got.name, "GeneralsXLauncher-macos-abc123456.tar.gz");
    }

    /// Linux, same shape: an engine repackaged as an AppImage published beside the launcher.
    #[test]
    fn a_linux_engine_appimage_is_not_mistaken_for_the_launcher_appimage() {
        let wanted = vec![
            row("linux", "GeneralsXZH-linux-x86_64-abc123456.AppImage"),
            row("linux", "GeneralsXLauncher-linux-x86_64-abc123456.AppImage"),
        ];
        let got = select_launcher_artifact(&wanted, "linux", &appimage()).expect("launcher present");
        assert_eq!(got.name, "GeneralsXLauncher-linux-x86_64-abc123456.AppImage");
    }

    /// AND WHEN THERE IS NO LAUNCHER ROW AT ALL, refusing is the answer.
    ///
    /// This is the arm that matters most: with only an engine row published, a suffix-only selector
    /// has nothing to fall back to and hands the engine over. `release_apply` turns `None` into a
    /// sentence and installs nothing, which is recoverable; the swap is not.
    #[test]
    fn an_engine_only_release_yields_nothing_rather_than_the_engine() {
        let wanted = vec![row("macos", "GeneralsXZH-macos-arm64-abc123456.tar.gz")];
        assert!(
            select_launcher_artifact(&wanted, "macos", &mac()).is_none(),
            "with no launcher published, apply must refuse rather than install the engine"
        );
    }

    /// An "any" row cannot be this platform's launcher however it is named. `decide` returns "any"
    /// rows to every platform, so the platform column is checked as well as the name.
    #[test]
    fn an_any_platform_row_is_not_this_platforms_launcher() {
        let wanted = vec![
            row("any", "GeneralsXLauncher-macos-abc123456.tar.gz"),
            row("any", "gxreq_dcfa195f.zip"),
        ];
        assert!(select_launcher_artifact(&wanted, "macos", &mac()).is_none());
    }

    /// The other platform's launcher is not ours, even when its suffix happens to fit the target.
    #[test]
    fn another_platforms_launcher_row_is_not_selected() {
        let wanted = vec![row("linux", "GeneralsXLauncher-linux-x86_64-abc123456.tar.gz")];
        assert!(select_launcher_artifact(&wanted, "macos", &mac()).is_none());
    }

    /// ONE RULE, TWO CALLERS. This is the whole finding stated as a property: the row the
    /// irreversible apply path selects must be one the display path would have counted as an
    /// update. Two predicates that agree today and drift tomorrow is what produced this.
    #[test]
    fn apply_never_selects_a_row_the_display_path_would_not_count() {
        let cases: Vec<(&str, Target, Vec<gxcore::Artifact>)> = vec![
            (
                "macos",
                mac(),
                vec![
                    row("macos", "GeneralsXZH-macos-arm64-abc123456.tar.gz"),
                    row("macos", "GeneralsXZH-macos-arm64-abc123456.zip"),
                    row("any", "gxreq_dcfa195f.zip"),
                    row("macos", "GeneralsXLauncher-macos-abc123456.tar.gz"),
                ],
            ),
            (
                "linux",
                appimage(),
                vec![
                    row("linux", "GeneralsXZH-linux-x86_64-abc123456.tar.gz"),
                    row("linux", "GeneralsXZH-linux-x86_64-abc123456.AppImage"),
                    row("any", "gxopt_3ae6628a.zip"),
                    row("linux", "GeneralsXLauncher-linux-x86_64-abc123456.AppImage"),
                ],
            ),
        ];

        for (platform, target, wanted) in cases {
            let displayed: Vec<String> = applicable_rows(wanted.clone(), platform)
                .into_iter()
                .map(|a| a.name)
                .collect();
            match select_launcher_artifact(&wanted, platform, &target) {
                Some(a) => assert!(
                    displayed.contains(&a.name),
                    "{platform}: apply would install {}, which the update panel never listed \
                     ({displayed:?})",
                    a.name
                ),
                None => assert!(
                    displayed.is_empty(),
                    "{platform}: the panel listed {displayed:?} but apply would install nothing"
                ),
            }
        }
    }

    /// THE OTHER HALF OF THE SAME DRIFT, in the direction that produced the original badge loop.
    ///
    /// `gxcore::decide` hands every platform the `any` rows. If the panel counted an `any` row that
    /// merely LOOKS like a launcher, it would report an update that `select_launcher_artifact`
    /// refuses to install - "update available" forever, on a release where pressing Update returns
    /// a sentence. So the platform column is checked on both sides, by the same function.
    #[test]
    fn the_panel_counts_no_row_the_apply_path_would_refuse() {
        let wanted = vec![
            row("any", "GeneralsXLauncher-macos-abc123456.tar.gz"),
            row("any", "gxreq_dcfa195f.zip"),
        ];
        assert!(
            applicable_rows(wanted.clone(), "macos").is_empty(),
            "the panel counted a row apply refuses, which is a badge that can never clear"
        );
        assert!(!applicable_row(&wanted[0], "macos"));
    }

    /// The suffix is a property of the TARGET FORM, not of the platform token. Pinned so the two
    /// cannot be quietly collapsed into one.
    #[test]
    fn the_target_form_decides_the_suffix() {
        assert_eq!(target_suffix(&mac()), ".tar.gz");
        assert_eq!(target_suffix(&appimage()), ".AppImage");
    }
}

#[cfg(test)]
mod tests {

    // ---- self-update on macOS and Linux -------------------------------------------------------
    //
    // The property under test is an ORDERING: nothing reaches the irreversible swap unhashed. The
    // swap itself lives in gxio and has its own tests; what could regress HERE is the gate in
    // front of it, and a gate is only proven by being made to refuse.

    // ---------------------------------------------------------------------------------------
    // Recording what was applied
    // ---------------------------------------------------------------------------------------
    //
    // The defect these pin: the Unix apply path used to record nothing, so `build_release_state`
    // produced the SAME verdict after a successful update as before it, and the launcher offered
    // the same update forever. The property is therefore not "a file was written" - it is that
    // `gxcore::decide` changes its mind.

    fn hashed_art(platform: &str, name: &str, hash: &str) -> gxcore::Artifact {
        gxcore::Artifact {
            platform: platform.to_string(),
            name: name.to_string(),
            hash: hash.to_string(),
            size: 1,
        }
    }

    fn hashed_manifest(ordinal: u64, artifacts: Vec<gxcore::Artifact>) -> gxcore::Manifest {
        gxcore::Manifest {
            version: 1,
            ordinal,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: String::new(),
            relays: Vec::new(),
            base: "dl.example/".to_string(),
            artifacts,
        }
    }

    #[test]
    fn recording_the_applied_artifact_stops_the_update_being_offered_again() {
        let launcher = hashed_art("macos", "GeneralsXLauncher-macos-abc123456.tar.gz", "HASH1");
        let m = hashed_manifest(1787151908, vec![launcher.clone()]);
        let before = gxcore::InstallState::default();

        assert!(
            matches!(gxcore::decide(&m, &before, "macos"), gxcore::Verdict::Update(_)),
            "with no record the update is offered - this is the state a shipped Unix install is in"
        );

        let after = record_one_applied(before, &launcher, m.ordinal);
        assert!(
            matches!(gxcore::decide(&m, &after, "macos"), gxcore::Verdict::UpToDate),
            "after recording the applied artifact the same manifest must read as up to date"
        );
        assert_eq!(after.ordinal, 1787151908, "the ordinal advanced to the manifest's");
    }

    #[test]
    fn recording_one_artifact_does_not_claim_the_others_are_installed() {
        // The C updater rebuilds the record from the whole manifest (gxlaunch.c:2168-2181) because
        // it applies every needed artifact. This arm applies exactly ONE. Copying that behaviour
        // would tell `decide` that an overlay nobody downloaded is up to date, and it would then
        // never be fetched - a silent failure to deliver, which is worse than a repeated offer.
        let launcher = hashed_art("macos", "GeneralsXLauncher-macos-abc123456.tar.gz", "HASH1");
        let overlay = hashed_art("any", "gxreq_deadbeef.zip", "HASH2");
        let m = hashed_manifest(9, vec![launcher.clone(), overlay.clone()]);

        let after = record_one_applied(gxcore::InstallState::default(), &launcher, m.ordinal);
        match gxcore::decide(&m, &after, "macos") {
            gxcore::Verdict::Update(needed) => {
                assert_eq!(needed.len(), 1, "exactly one artifact is still needed");
                assert_eq!(needed[0].name, overlay.name, "and it is the one nobody applied");
            }
            other => panic!("the un-applied artifact must still be needed, got {other:?}"),
        }
    }

    #[test]
    fn re_applying_updates_the_hash_in_place_rather_than_adding_a_second_row() {
        // A publisher can correct a hash at the same ordinal - `decide` uses strict `<` precisely
        // so that a re-publish still applies. Two rows for one artifact would make `has()` return
        // whichever came first, which may be the stale one.
        let v1 = hashed_art("macos", "L.tar.gz", "OLD");
        let v2 = hashed_art("macos", "L.tar.gz", "NEW");
        let st = record_one_applied(gxcore::InstallState::default(), &v1, 5);
        let st = record_one_applied(st, &v2, 5);
        assert_eq!(st.items.len(), 1, "one artifact, one row");
        assert_eq!(st.items[0].hash, "NEW");
        assert!(matches!(
            gxcore::decide(&hashed_manifest(5, vec![v2]), &st, "macos"),
            gxcore::Verdict::UpToDate
        ));
    }

    #[test]
    fn the_recorded_ordinal_never_moves_backwards() {
        // `decide` refuses a manifest older than the record. If applying could lower the ordinal,
        // that rollback floor could be walked down one release at a time.
        let a = hashed_art("macos", "L.tar.gz", "H");
        let st = gxcore::InstallState { ordinal: 100, ..Default::default() };
        let st = record_one_applied(st, &a, 7);
        assert_eq!(st.ordinal, 100, "a lower manifest ordinal must not lower the record");
    }

    #[test]
    fn a_recorded_state_round_trips_through_the_file_format() {
        // The record is written with render() and read back with parse(). A field that does not
        // survive that round trip is a field the next launch does not see.
        let a = hashed_art("macos", "GeneralsXLauncher-macos-abc123456.tar.gz", "HASH1");
        let st = record_one_applied(gxcore::InstallState::default(), &a, 42);
        let back = gxcore::InstallState::parse(&st.render());
        assert_eq!(back, st, "what is written is what is read");
        assert!(matches!(
            gxcore::decide(&hashed_manifest(42, vec![a]), &back, "macos"),
            gxcore::Verdict::UpToDate
        ));
    }

    fn staged(bytes: &[u8]) -> std::path::PathBuf {
        let mut p = std::env::temp_dir();
        p.push(format!("gxverify-{}-{}", std::process::id(), bytes.len()));
        std::fs::write(&p, bytes).unwrap();
        p
    }

    #[test]
    fn a_matching_hash_is_accepted_and_the_file_survives() {
        let body = b"the artifact bytes";
        let path = staged(body);
        let mut h = gxcore::ArtifactHasher::new();
        h.update(body);
        let good = h.finish();

        assert!(gxio::update::verify_staged(&path, "artifact.tar.gz", &good).is_ok());
        assert!(path.exists(), "a verified download must NOT be deleted - it is about to be applied");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_hash_mismatch_is_refused_and_the_file_is_removed() {
        let path = staged(b"bytes that will not hash to the expected value");
        let err = gxio::update::verify_staged(&path, "artifact.tar.gz", "AAAAAAAAAAAAAAAAAAAAAA")
            .expect_err("a mismatched artifact must be refused BEFORE any swap");

        assert!(err.contains("does not match the signed release"), "unhelpful message: {err}");
        assert!(err.contains("Nothing was installed"), "must say nothing was installed: {err}");
        assert!(
            !path.exists(),
            "a refused artifact must be removed, or the next attempt finds it and skips the download"
        );
    }

    #[test]
    fn an_unreadable_staged_file_is_refused_rather_than_treated_as_verified() {
        let missing = std::env::temp_dir().join("gxverify-does-not-exist-at-all");
        let _ = std::fs::remove_file(&missing);
        let err = gxio::update::verify_staged(&missing, "artifact.AppImage", "AAAAAAAAAAAAAAAAAAAAAA")
            .expect_err("an unreadable download must not be treated as verified");
        assert!(err.contains("could not be read back"), "unhelpful message: {err}");
    }

    use super::*;

    #[test]
    fn the_commit_is_the_last_dash_token_of_an_artifact_name() {
        assert_eq!(
            commit_in_artifact_name("GeneralsX-win64-57d14a20b.zip").as_deref(),
            Some("57d14a20b")
        );
        // The extra token after the commit is the shipped-and-live trap the delivery plan is
        // built around: state_load compares the text after the LAST dash.
        assert_eq!(
            commit_in_artifact_name("GeneralsXZH-linux-x86_64-57d14a20b-dafa815d.tar.gz").as_deref(),
            Some("dafa815d")
        );
    }

    #[test]
    fn a_tier_file_contributes_no_commit_at_all() {
        // gxrt_66dd5434.zip has no dash. A greedy rule would offer "66dd5434" as the engine
        // commit, which is a content hash of a different tier entirely.
        assert!(commit_in_artifact_name("gxrt_66dd5434.zip").is_none());
        assert!(commit_in_artifact_name("gxreq_dcfa195f.zip").is_none());
        assert!(commit_in_artifact_name("notes-README.txt").is_none());
    }

    #[test]
    fn tier_labels_match_the_windows_launchers_words() {
        assert_eq!(tier_label("gxrt_66dd5434.zip"), "runtime (launcher and DLLs)");
        assert_eq!(tier_label("gxreq_dcfa195f.zip"), "required game data");
        assert_eq!(tier_label("gxmdl_5c1e0a77.zip"), "required game data (3D models)");
        assert_eq!(tier_label("gxopt_3ae6628a.zip"), "optional extras");
        assert_eq!(tier_label("GeneralsX-win64-57d14a20b.zip"), "engine");
        // Unknown tier: its own name and its real size, which is accurate and merely less pretty.
        assert_eq!(tier_label("something-new.zip"), "something-new.zip");
    }

    #[test]
    fn the_launcher_row_is_called_the_launcher_and_not_the_engine() {
        // The home screen paints the macOS update verdict in place now, and its sentence names each
        // row by this label: "engine 7.9 MB" about the launcher's own update was false.
        assert_eq!(tier_label("GeneralsXLauncher-macos-fc33f7efd.tar.gz"), "launcher");
        assert_eq!(tier_label("GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage"), "launcher");
        assert_eq!(tier_label("GeneralsZH-macos-arm64-55739fcf9.zip"), "engine");
    }

    #[test]
    fn megabytes_are_one_decimal() {
        assert_eq!(mb(0), "0.0 MB");
        assert_eq!(mb(4_506_943), "4.3 MB");
        assert_eq!(mb(64_805_172), "61.8 MB");
    }

    #[test]
    fn a_document_that_does_not_verify_yields_no_manifest() {
        // One flipped byte in the ordinal of a real published document.
        const LIVE: &str = "2|1787214478|0|V9FKIL5k|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-57d14a20b.zip,RWg-mePOEOlLzD6jooTxVg,4506943|umttokm0hOr4lxZstlcsY1bsYjDzFRcWpQc66zfYVJGUUXhWwkmivp1c7lXf59mHd245q6S4jPifPu1uj0t_Dg";
        let tampered = LIVE.replacen("1787214478", "1787214479", 1);
        assert!(verified(&tampered).is_none());
    }

    #[test]
    fn an_error_page_never_reaches_verify() {
        // It would come back as SignatureMismatch, and "the CDN 404'd" would reach a player as
        // "these bytes are not authentic".
        assert!(matches!(
            rel::http_document("<!DOCTYPE html><title>404</title>"),
            rel::Framed::Unexpected
        ));
    }

    fn rel_entry(ordinal: u64, commit: &str) -> rel::ChangeRelease {
        rel::ChangeRelease {
            ordinal,
            commit: commit.into(),
            date: "2026-08-20".into(),
            title: "t".into(),
            notes: Vec::new(),
        }
    }

    /// INSTALLED ONLY WHEN THE EVIDENCE NAMES ONE RELEASE - and only a record's ORDINAL does
    /// (Codex, sixth, eighth and ninth rounds). A commit - from BUILD.txt, the launcher's own
    /// install, or the engine binary - cannot: the live changelog of 18/09/2026 publishes six commits
    /// at more than one ordinal (55739fcf9 three times); a cached or capped list cannot show a commit
    /// is unique; and after a launcher-only release the engine's commit IS unique, but in the OLDER
    /// release's entry, while the install belongs to the newer one. Three rules were tried for
    /// commit evidence and each had a hole, so commit evidence is SAME COMMIT, always.
    #[test]
    fn installed_is_said_only_when_the_evidence_names_one_release() {
        let entries = || vec![rel_entry(3, "55739fcf9"), rel_entry(2, "55739fcf9"), rel_entry(1, "8ff29efa7")];
        let id = |from, commit: &str| Identity { commit: commit.into(), from, binary: None };
        let exact = |entries: Vec<rel::ChangeRelease>, source, from, commit: &str, ordinal| {
            let d = changelog_dto(entries, source, &id(from, commit), ordinal);
            assert_eq!(d.entries.iter().filter(|e| e.installed).count(), 1, "{from:?} {commit}");
            d.installed_exact
        };
        for source in ["network", "cache"] {
            assert!(exact(entries(), source, InstalledFrom::Record, "55739fcf9", 2), "a record's ordinal names one release");
        }
        for from in [InstalledFrom::Build, InstalledFrom::Installer, InstalledFrom::Binary] {
            assert!(!exact(entries(), "network", from, "55739fcf9", 0), "{from:?}: a repeated commit");
            assert!(!exact(entries(), "network", from, "8ff29efa7", 0), "{from:?}: a commit, even a unique one");
        }
        // The launcher-only release: its entry carries the launcher's commit, the engine's commit is
        // unique - in the previous release's entry.
        let launcher_only = vec![rel_entry(5, "4b38914f1"), rel_entry(4, "8ff29efa7")];
        assert!(!exact(launcher_only, "network", InstalledFrom::Installer, "8ff29efa7", 0), "the older release marked exact");

        let page = include_str!("../../../../design/launcher/screens/Changelogs.html");
        let at = page.find("function paintBadge(badge, e) {").expect("paintBadge is gone");
        let body = &page[at..at + page[at..].find("\n  }\n").expect("paintBadge closes")];
        assert!(
            body.contains("installedExact ? 'INSTALLED' : 'SAME COMMIT'") && !body.contains("installedFrom === 'binary';"),
            "Changelogs words the badge by a rule of its own:\n{body}"
        );
    }

    #[test]
    fn a_repeated_commit_does_not_mark_two_entries_installed() {
        // The live document published 20/08/2026 really does carry 258405c20 twice and
        // 53b8e04e3 twice: a re-publish at a new ordinal ships the same engine commit. Marking on
        // the commit alone put an INSTALLED badge on both, and the reader could not tell which
        // one they had.
        let e = vec![
            rel_entry(1787139667, "258405c20"),
            rel_entry(1787136880, "258405c20"),
        ];
        // No record: only the NEWEST entry carrying the commit is marked.
        assert_eq!(which_entry_is_installed(&e, 0, "258405c20"), Some(0));
    }

    #[test]
    fn the_ordinal_wins_over_the_commit_whenever_there_is_a_record() {
        // BUILD.txt can name the newest engine while the record sits at an older ordinal - the
        // launcher rides the runtime tier, so a release can move it without moving the engine.
        let e = vec![rel_entry(300, "aaaaaaaaa"), rel_entry(200, "bbbbbbbbb")];
        assert_eq!(which_entry_is_installed(&e, 200, "aaaaaaaaa"), Some(1));
        // An ordinal that names no published entry marks nothing, rather than falling back to a
        // commit that would mark the wrong row.
        assert_eq!(which_entry_is_installed(&e, 999, "aaaaaaaaa"), None);
    }

    #[test]
    fn with_nothing_measured_nothing_is_marked() {
        let e = vec![rel_entry(300, "aaaaaaaaa")];
        assert_eq!(which_entry_is_installed(&e, 0, ""), None);
    }

    #[test]
    fn an_unknown_state_is_never_phrased_as_up_to_date() {
        let s = build_release_state(false);
        if s.verdict == "unknown" {
            assert!(
                !s.detail.to_lowercase().contains("up to date"),
                "detail promised currency it cannot know: {}",
                s.detail
            );
        }
        assert!(matches!(
            s.verdict.as_str(),
            "unknown" | "uptodate" | "update" | "older" | "unmanaged"
        ));
        // "fixture": a fixture build run with GX_SELFUPDATE_FIXTURE judges the fixture's document
        // (self_update::fixture_world), and only such a build can say so (Codex q7-macupd-r6).
        let fixture_source = cfg!(gx_selfupdate_fixture) && s.source == "fixture";
        assert!(matches!(s.source.as_str(), "network" | "relay" | "cache" | "none") || fixture_source, "{}", s.source);
    }

    // --- what identifies an install, and what that lets a verdict say -----------------------

    fn art(platform: &str, name: &str, size: u64) -> gxcore::Artifact {
        gxcore::Artifact {
            platform: platform.into(),
            name: name.into(),
            hash: "AAAAAAAAAAAAAAAAAAAAAA".into(),
            size,
        }
    }

    /// The live document's own shape, 21/08/2026: `m-7FcX8v` is the signed `sha` field and decodes
    /// to the commit every artifact in it is named after.
    fn live_manifest() -> gxcore::Manifest {
        gxcore::Manifest {
            version: 2,
            ordinal: 1787236345,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: "m-7FcX8v".into(),
            relays: Vec::new(),
            base: "dl.wh1ter0se.dev/".into(),
            artifacts: vec![
                art("macos", "GeneralsXLauncher-macos-9beec5717.tar.gz", 3_172_082),
                art("any", "gxreq_dcfa195f.zip", 64_805_172),
            ],
        }
    }

    fn wants_everything() -> gxcore::Verdict {
        gxcore::Verdict::Update(live_manifest().artifacts)
    }

    /// What `outstanding` returns when nothing this launcher can apply is outstanding - the
    /// usual case, record or not. Until the fourth review the tests below fed `wants_everything()`
    /// here, standing for `decide` over an EMPTY record; that arithmetic over nothing is no longer
    /// what any arm sees (see `outstanding`).
    fn nothing_outstanding() -> gxcore::Verdict {
        gxcore::Verdict::UpToDate
    }

    /// A Windows document in the live shape: the engine bundle and the tiers, and no launcher row
    /// (on Windows the launcher rides the runtime tier).
    fn windows_manifest() -> gxcore::Manifest {
        gxcore::Manifest {
            artifacts: vec![
                art("win32", "GeneralsX-win64-9beec5717.zip", 4_492_409),
                art("win32", "gxrt_66dd5434.zip", 63_948_459),
                art("any", "gxreq_58c3de93.zip", 29_204_191),
            ],
            ..live_manifest()
        }
    }

    /// The verdict `build_release_state` computes for a Windows install with NO record - a fresh
    /// extract of the share zip, before the updater has run.
    fn windows_no_record_kind(build_commit: &str) -> (&'static str, u64) {
        let m = windows_manifest();
        let dev = crate::self_update::OwnBuild { commit9: "aaaaaaaaa", commit_epoch: None };
        let verdict = outstanding(&m, None, "win32", dev, build_commit, false, None);
        let kind = verdict_kind(&verdict, InstalledFrom::Build, build_commit, "9beec5717");
        (kind, payload(&verdict, kind).1)
    }

    #[test]
    fn the_sha_field_of_the_live_document_is_the_commit_the_test_fixtures_use() {
        // Pins the fixture to the real thing rather than to a number I typed. If this fails, the
        // three verdict tests below are comparing strings that no document ever carried.
        let m = live_manifest();
        let c = rel::commit_hex_from_sha_field(&m.sha).expect("eight base64url characters");
        assert_eq!(rel::short_commit(&c), "9beec5717");
    }

    #[test]
    fn a_build_stamp_matching_the_published_release_still_offers_the_updaters_plan_with_no_record() {
        // THIS USED TO SAY "Up to date" (9c5c0e921), on the premise that a Windows install with no
        // record never gets one. Since 19/08 every bundle ships its record, so no record means it
        // was deleted - the record's own header says "delete to force a re-download" - or it
        // predates that. BUILD.txt rides the ENGINE tier only and vouches for nothing else, and
        // Update is the one way this launcher runs GeneralsXUpdate.exe, which with no record
        // fetches every row. Hiding the button stranded that repair (Codex, fifth review, P1).
        let everything = 4_492_409 + 63_948_459 + 29_204_191;
        assert_eq!(windows_no_record_kind("9beec5717"), ("update", everything));
    }

    #[test]
    fn a_build_stamp_behind_the_published_release_is_a_real_update() {
        // 57d14a20b was the release before 9beec5717. GeneralsXUpdate.exe, with no record, fetches
        // every row the platform wants - 93.1 MB here, as the screen prints it - and that is what
        // the screen now says. (The fourth review had it `unmanaged`; the fifth found the filter
        // that made it so.)
        let everything = 4_492_409 + 63_948_459 + 29_204_191;
        assert_eq!(windows_no_record_kind("57d14a20b"), ("update", everything));
        // An install whose BUILD.txt is unreadable is not silently treated as a match.
        assert_eq!(windows_no_record_kind(""), ("update", everything));
        // What IS outstanding and applicable still stands, match or not.
        for c in ["57d14a20b", "9beec5717", ""] {
            assert_eq!(verdict_kind(&wants_everything(), InstalledFrom::Build, c, "9beec5717"), "update", "{c}");
        }
    }

    #[test]
    fn a_build_stamp_ahead_of_the_document_does_not_swallow_the_older_verdict() {
        // `older` belongs to the ORDINAL comparison in gxcore::decide, and a build stamp carries
        // no ordinal - so this arm exists to prove the BUILD.txt path passes that answer through
        // rather than rewriting it as an update or as currency. See the report note: with no
        // record the installed ordinal is 0, so `decide` cannot reach this on a build-stamp-only
        // install today, and nothing here invents an ordinal to make it reachable.
        let ahead = gxcore::Verdict::OlderThanInstalled { manifest: 1787214478, installed: 1787236345 };
        assert_eq!(verdict_kind(&ahead, InstalledFrom::Build, "9beec5717", "57d14a20b"), "older");
        assert_eq!(verdict_kind(&ahead, InstalledFrom::Record, "9beec5717", "57d14a20b"), "older");
    }

    #[test]
    fn a_record_leaves_decides_answer_exactly_as_it_was() {
        // The record carries an ordinal AND a hash per artifact, so `decide` already knows more
        // than a commit comparison could add. Even a commit that matches must not turn a real
        // outstanding artifact - a moved runtime tier at an unchanged engine commit - into
        // "nothing will download".
        assert_eq!(
            verdict_kind(&wants_everything(), InstalledFrom::Record, "9beec5717", "9beec5717"),
            "update"
        );
        assert_eq!(
            verdict_kind(&gxcore::Verdict::UpToDate, InstalledFrom::Record, "", "9beec5717"),
            "uptodate"
        );
    }

    #[test]
    fn neither_file_is_still_unmanaged_and_the_sentence_no_longer_reads_as_a_fault() {
        assert_eq!(
            verdict_kind(&nothing_outstanding(), InstalledFrom::Nothing, "", "9beec5717"),
            "unmanaged"
        );

        let nothing = Identity { commit: String::new(), from: InstalledFrom::Nothing, binary: None };
        let d = describe("unmanaged", &wants_everything(), &[], 0, false, "network", "9beec5717", &nothing);

        // It still says what is published - that is a fact and the screen shows it either way.
        assert!(d.contains("9beec5717"), "{d}");
        // It says WHY nothing is shown, as a statement of what is absent rather than a fault.
        //
        // RETIRED 18/09/2026: this used to assert the phrase "normal for a build run from a
        // development tree". That stopped being true the day the engine binary became a third
        // source - a development build carries a gitinfo stamp and now lands in the `binary` arm,
        // so reaching THIS arm means something unusual, and calling it normal would be the
        // confidently-wrong sentence the module refuses. The voice checks below are unchanged.
        assert!(d.contains("names this build"), "{d}");
        // ONE sentence, measured, no exclamation - the module's voice.
        assert_eq!(d.matches('.').count(), 1, "one sentence: {d}");
        assert!(!d.contains('!'), "{d}");
        // "cannot" was the word that made it read as a failure report.
        assert!(!d.to_lowercase().contains("cannot"), "{d}");
        // It never promises currency either; unmanaged knows less than uptodate does.
        assert!(!d.to_lowercase().contains("up to date"), "{d}");
    }

    #[test]
    fn no_document_is_unknown_whatever_the_install_turns_out_to_be() {
        // Unchanged, and structurally so: unknown_kind takes no argument about the install, so no
        // amount of BUILD.txt evidence can promote "we obtained nothing" into a statement about
        // currency. That promotion is the exact bug UpdateStatus.cs was written to remove.
        for allow_network in [false, true] {
            let (kind, detail) = unknown_kind(allow_network);
            assert_eq!(kind, "unknown");
            assert!(!detail.to_lowercase().contains("up to date"), "{detail}");
            assert!(!detail.is_empty());
        }
    }

    #[test]
    fn the_installed_evidence_field_agrees_with_the_files_behind_it() {
        // Run against whatever this host happens to have. Every invariant has to hold on a
        // development tree, on a fresh share-zip extract, and on a fully updated install alike -
        // and because a host reaches only one of those, the truth table runs the same invariants
        // over every row it has (Codex, fifth round).
        release_state_invariants(&build_release_state(false));
    }

    /// What every release state must satisfy, whatever install produced it.
    pub(super) fn release_state_invariants(s: &ReleaseStateDto) {
        assert!(matches!(s.installed_from.as_str(), "record" | "build" | "binary" | "installer" | "none"));
        // has_record still means exactly "gxinstalled.txt exists" and nothing else. `record` implies
        // it; the reverse stopped holding 18/09/2026, when a launcher-only record stopped counting
        // as evidence about the engine.
        if s.installed_from == "record" {
            assert!(s.has_record, "a record identified the engine, yet has_record is false");
        }
        if s.installed_from == "none" {
            assert!(s.installed_commit.is_empty());
            assert_eq!(s.installed_ordinal, 0);
        }
        if matches!(s.installed_from.as_str(), "build" | "binary" | "installer") {
            assert!(!s.installed_commit.is_empty());
            // Neither a build stamp nor an engine's gitinfo carries an ordinal. Zero is the
            // absence, never a date.
            assert_eq!(s.installed_ordinal, 0);
        }
        // "Players can join" is never shown for an install nothing identifies, and never the old
        // fixed sample on any install.
        if s.installed_from == "none" {
            assert!(s.joins.is_empty(), "{}", s.joins);
        }
        assert!(!s.joins.to_lowercase().contains("any platform"), "{}", s.joins);
        // The three fields the binary source adds are EMPTY unless it is the source. A release
        // install must never carry a commit date or a "recorded" sha it did not measure.
        if s.installed_from != "binary" && s.installed_from != "installer" {
            assert!(s.installed_built.is_empty(), "{}", s.installed_built);
            assert!(s.recorded_commit.is_empty(), "{}", s.recorded_commit);
            assert_eq!(s.installed_unpublished, None);
        }
        // `unmanaged` means nothing ties this install to the published release: either nothing
        // identifies it, or the engine names a commit that is not the published one.
        //
        // WIDENED 18/09/2026 from `== "none"`. That was true while the engine binary was never
        // read; it stopped being true when the binary became a source, and this test is what
        // caught it - run on the owner's Mac it reported `binary` on a real hand-installed
        // development engine, which is the fix working rather than a fault. And `build` since
        // fa72c21c9: a Linux BUILD.txt behind a current launcher is `unmanaged` (row R5), which only
        // a Linux host could reach - so the truth table now runs this too (Codex, fifth round).
        if s.verdict == "unmanaged" {
            assert!(matches!(s.installed_from.as_str(), "none" | "build" | "binary" | "installer"), "{}", s.installed_from);
        }
        // A verdict that does not promise a download must not be carrying one.
        if s.verdict != "update" {
            assert!(s.items.is_empty(), "{} carried {} items", s.verdict, s.items.len());
            assert_eq!(s.total_bytes, 0);
        }
    }

    #[test]
    fn the_offline_paths_invent_nothing() {
        // Run on a machine with whatever it happens to have. The invariant that must hold
        // everywhere: a field is either measured or empty, never a placeholder.
        let s = build_release_state(false);
        // A fixture document is one launcher row with an ordinal and no engine, so it has no engine
        // commit by construction (fixture builds only; Codex q7-macupd-r6).
        if s.available_commit.is_empty() && !(cfg!(gx_selfupdate_fixture) && s.source == "fixture") {
            assert_eq!(s.available_ordinal, 0, "a commit with no document behind it");
        }
        if !s.available_commit.is_empty() {
            assert_eq!(s.available_commit.len(), 9);
            assert!(s.available_commit.bytes().all(|b| b.is_ascii_hexdigit()));
        }
        if !s.has_record {
            assert_eq!(s.installed_ordinal, 0);
        }

        let c = build_changelog(false);
        assert!(matches!(c.source.as_str(), "network" | "cache" | "none"));
        // The two screens read ONE identity: the changelog's account of the install is the home
        // screen's, field for field.
        assert_eq!(c.installed_from, s.installed_from, "the two screens disagree on the evidence");
        assert_eq!(c.installed_commit, s.installed_commit, "the two screens disagree on the build");
        assert_eq!(c.installed_ordinal, s.installed_ordinal);
        if c.source == "none" {
            assert!(c.entries.is_empty(), "no source must mean no entries");
        }
        // Nothing may be marked as installed unless something measured says so.
        if c.installed_ordinal == 0 && c.installed_commit.is_empty() {
            assert!(c.entries.iter().all(|e| !e.installed));
        }
    }

    #[test]
    fn about_reports_this_binarys_own_stamps() {
        let a = about_info().expect("about never fails on a host with a home directory");
        assert_eq!(a.launcher_version, env!("CARGO_PKG_VERSION"));
        assert_eq!(a.launcher_commit, env!("GX_COMMIT9"));
        assert!(!a.launcher_commit.is_empty());
        assert!(a.launcher_commit.len() <= 9);
        assert!(matches!(a.platform.as_str(), "win32" | "macos" | "linux"));
        assert!(!a.state_dir.is_empty());
        // The three join gates are measured or absent - never a placeholder like the artboard's
        // 16-bit "0x2A17", which matches nothing this engine produces.
        for v in [&a.source_id, &a.engine_id, &a.data_id] {
            assert!(v.is_empty() || v.bytes().all(|b| b.is_ascii_hexdigit()));
        }
        if a.source_id.is_empty() && a.engine_id.is_empty() && a.data_id.is_empty() {
            assert!(a.sim_from.is_empty(), "a source with no values behind it");
        }
    }

    /// THE HOME SCREEN NAMES THE LAUNCHER, NOT ONLY THE GAME. The install card carried the ENGINE's
    /// build and said nothing about the launcher, so "which launcher am I running" had no answer on
    /// the screen a player actually opens - only on the About page, two clicks away. Asked for by the
    /// owner on 13/09/2026, looking at his own home screen: "So we show game version and launcher
    /// version".
    ///
    /// The row is painted from `about_info`, which this page already invokes, rather than written
    /// into the markup: a version compiled into HTML is a version that goes stale the first time
    /// Cargo.toml moves, and nothing would catch it.
    #[test]
    fn the_home_screen_shows_the_launchers_own_version() {
        let html = include_str!("../../../../design/launcher/screens/MainWeb.html");
        assert!(html.contains(">Launcher<"), "the install card has no Launcher row");
        assert!(
            html.contains("valueAfter('Launcher')"),
            "nothing reads the Launcher row's value element, so the row can only be static text"
        );
        assert!(
            html.contains("launcher_version"),
            "the Launcher row is not painted from about_info's launcher_version"
        );
    }

    /// A REAL VERSION, NOT THE SCAFFOLD 0.1.0, AND THE TWO FILES AGREE. The owner's call on
    /// 13/09/2026: SemVer-ish, he sets the number, the commit carried as build metadata - "pls make
    /// the game actually have versions not wierd ass commit names".
    ///
    /// All three matter and they are read by different things: `Cargo.toml` is what
    /// `CARGO_PKG_VERSION` reports to `about_info` and the selftest line, `tauri.conf.json` is what
    /// the bundle and the .dmg filename carry. Letting them drift would put one number on the screen
    /// and a different one on the file the player downloaded.
    /// A LAUNCHER ON A TEST PROFILE NEVER RUNS THE WINDOWS UPDATER (Codex r2 of 2aa151364, P1): it
    /// ignores GX_USERDATA_DIR and would read and write the owner's profile. Windows-only code, so read:
    /// both callers take the exe from `updater_exe`, whose first act is the override check, and nothing
    /// else in the file names the updater's exe. Red arms: drop the check, or re-inline either caller.
    #[test]
    fn a_test_profile_never_runs_the_windows_updater() {
        let prod = include_str!("cmd_release.rs").split("#[cfg(test)]").next().unwrap();
        let sq = |s: &str| s.split_whitespace().collect::<String>();
        let helper = prod.split("fn updater_exe() -> Result<(std::path::PathBuf, std::path::PathBuf), String> {").nth(1).expect("updater_exe");
        assert!(sq(helper).starts_with("ifgxio::release::user_data_dir_override().is_some(){returnErr("), "updater_exe must refuse first");
        assert_eq!(prod.matches("\"GeneralsXUpdate.exe\")").count(), 1, "one place names the updater: updater_exe");
        assert!(sq(prod).contains("let(dir,exe)=updater_exe()?;"), "release_apply must go through updater_exe");
        assert!(sq(prod).contains("let(dir,exe)=updater_exe().ok()?;"), "updater_check_plan must go through updater_exe");
    }

    #[test]
    fn the_launcher_version_is_real_and_all_three_files_agree() {
        let v = env!("CARGO_PKG_VERSION");
        assert_ne!(v, "0.1.0", "the launcher still carries the scaffold version");
        let conf = include_str!("../tauri.conf.json");
        assert!(
            conf.contains(&format!("\"version\": \"{v}\"")),
            "tauri.conf.json does not carry {v}, so the bundle and CARGO_PKG_VERSION disagree"
        );
        // GeneralsX @bugfix Claude 21/09/2026 THERE IS A THIRD FILE, and this test covered two.
        //
        // scripts/build/launcher/toolchain.env carries GX_APP_VERSION, which every packager sources
        // and which names the .dmg the player downloads. check-subsystem2.sh does assert it equals
        // tauri.conf.json's - but that gate runs at PACKAGING time, on a release host, which is the
        // most expensive possible moment to learn that three files disagree. Found by bumping 0.9.0
        // to 0.9.41 and noticing this one stayed behind while 268 tests stayed green.
        let env = include_str!("../../../build/launcher/toolchain.env");
        assert!(
            env.lines().any(|l| l.trim() == format!("GX_APP_VERSION={v}")),
            "toolchain.env does not carry GX_APP_VERSION={v}, so the packagers and the binary disagree"
        );
    }

    // -----------------------------------------------------------------------------------------
    // The engine binary as a third source (18/09/2026)
    // -----------------------------------------------------------------------------------------

    fn stamp(commit: &str, built: &str) -> gxio::enginestamp::EngineStamp {
        gxio::enginestamp::EngineStamp {
            commit: commit.into(),
            short: commit.chars().take(9).collect(),
            built: built.into(),
        }
    }

    /// A stamp as `find_engine_stamp` returns it: with the file it was read from.
    fn at_bin(s: gxio::enginestamp::EngineStamp) -> (std::path::PathBuf, gxio::enginestamp::EngineStamp) {
        ("/x/GeneralsXZH.app/Contents/Resources/bin/GeneralsXZH".into(), s)
    }

    const DEV: &str = "3fd38c2daf2a77e1c9301dcd53e0d49613f2d397";
    const REL: &str = "8ff29efa7";

    #[test]
    fn the_binary_is_asked_only_when_nothing_better_answered() {
        // A record wins outright, and the stamp is not even consulted.
        let id = identity_from(true, "9beec5717".into(), Some(stamp(DEV, "")), None, None);
        assert_eq!(id.from, InstalledFrom::Record);
        assert_eq!(id.binary, None);
        // BUILD.txt wins over the binary: a release install is answered exactly as before.
        let id = identity_from(false, "9beec5717".into(), Some(stamp(DEV, "")), None, None);
        assert_eq!(id.from, InstalledFrom::Build);
        assert_eq!(id.commit, "9beec5717");
        assert_eq!(id.binary, None);
        // Neither: the binary answers.
        let id = identity_from(false, String::new(), Some(stamp(DEV, "x")), None, None);
        assert_eq!(id.from, InstalledFrom::Binary);
        assert_eq!(id.commit, "3fd38c2da", "the nine-character form the panel prints");
        // Nothing at all.
        let id = identity_from(false, String::new(), None, None, None);
        assert_eq!(id.from, InstalledFrom::Nothing);
        assert_eq!(id.commit, "");
    }

    /// THE OWNER'S MAC, MEASURED 18/09/2026: engine 3fd38c2da, record 8ff29efa7, published 8ff29efa7,
    /// and 3fd38c2da in no release. Every field the panel needs, from facts on disk.
    #[test]
    fn a_hand_installed_development_engine_is_named_and_the_stale_record_is_reported() {
        let releases = vec![REL.to_string()];
        let id = identity_from(
            false,
            String::new(),
            Some(stamp(DEV, "2026-09-15T20:48:29+03:00")),
            Some(REL.into()),
            Some(&releases),
        );
        let b = id.binary.as_ref().expect("binary evidence");
        assert_eq!(id.commit, "3fd38c2da");
        assert_eq!(b.built, "2026-09-15T20:48:29+03:00");
        assert_eq!(b.recorded, "8ff29efa7", "the disagreeing record is carried to the screen");
        assert_eq!(b.unpublished, Some(true));

        assert_eq!(verdict_kind(&nothing_outstanding(), id.from, &id.commit, REL), "unmanaged");
        let d = unmanaged_detail(REL, &id);
        assert_eq!(
            d,
            "Development build 3fd38c2da, not the published release 8ff29efa7; the launcher \
             recorded installing 8ff29efa7 here, so this engine was replaced outside it."
        );
    }

    #[test]
    fn a_record_that_agrees_with_the_binary_is_not_reported() {
        let id = identity_from(false, String::new(), Some(stamp(DEV, "")), Some("3fd38c2da".into()), None);
        assert_eq!(id.binary.unwrap().recorded, "", "agreement adds nothing the Build row lacks");
    }

    #[test]
    fn development_build_is_said_only_when_a_changelog_was_read() {
        // No changelog: the commit could be an old release, so the stronger phrase is withheld.
        let id = identity_from(false, String::new(), Some(stamp(DEV, "")), None, None);
        assert_eq!(id.binary.as_ref().unwrap().unpublished, None);
        let d = unmanaged_detail(REL, &id);
        assert!(!d.contains("Development"), "{d}");
        assert!(d.contains("not the published release 8ff29efa7"), "{d}");

        // A changelog that DOES carry it: the COMMIT of an old release - and named as that commit,
        // not as "release 3fd38c2da", for the reason the next test gives. Until 18/09/2026 this
        // read "This engine is release 3fd38c2da", which is the claim a commit alone cannot back.
        let rs = vec![REL.to_string(), "3fd38c2da".to_string()];
        let id = identity_from(false, String::new(), Some(stamp(DEV, "")), None, Some(&rs));
        assert_eq!(id.binary.as_ref().unwrap().unpublished, Some(false));
        let d = unmanaged_detail(REL, &id);
        assert!(d.starts_with("Built from 3fd38c2da, the commit of another release"), "{d}");
        assert!(!d.contains("is release"), "a commit is not a release build: {d}");
    }

    /// F4, CODEX 18/09/2026: A COMMIT MATCH FROM THE BINARY IS NOT THE RELEASE. A development engine
    /// built AT the published commit - with uncommitted changes, or other build flags - carries the
    /// same gitinfo as the release, and this arm used to call it "uptodate" and list the release's
    /// platforms under "Players can join". Measured on the owner's Mac, nothing else in the binary
    /// separates the two: the dirty flag reads 1 on the release engine as well, and the source
    /// digest is identical and published nowhere. So the commit is reported, and the release is not
    /// claimed.
    #[test]
    fn a_binary_at_the_published_commit_is_not_called_the_release() {
        let kind = verdict_kind(&nothing_outstanding(), InstalledFrom::Binary, REL, REL);
        assert_eq!(kind, "unmanaged", "a commit match from the binary claimed to be the release");
        assert_eq!(
            who_can_join(kind, REL, REL, &live_rows()),
            "matching builds only",
            "the release's platforms were listed for a build only known by its commit"
        );

        let id = identity_from(false, String::new(), Some(stamp(REL, "")), None, None);
        let d = unmanaged_detail(REL, &id);
        assert_eq!(
            d,
            "Built from 8ff29efa7, the published release's commit, but nothing on this disk shows it \
             was built the way that release was."
        );
        // Whatever the changelog says about the commit, the release is not claimed.
        let rs = vec![REL.to_string()];
        let id = identity_from(false, String::new(), Some(stamp(REL, "")), None, Some(&rs));
        assert!(!unmanaged_detail(REL, &id).contains("is release"));

        // A different commit is not an update either: with no record, `decide`'s rows are the
        // launcher tier, which says nothing about this engine.
        assert_eq!(
            verdict_kind(&nothing_outstanding(), InstalledFrom::Binary, "3fd38c2da", REL),
            "unmanaged"
        );
        assert_eq!(
            verdict_kind(&nothing_outstanding(), InstalledFrom::Binary, "", REL),
            "unmanaged",
            "an empty commit never matches"
        );
    }

    /// THE RELEASE PATH DID NOT MOVE. `BUILD.txt` is the packager's own statement about the binary
    /// it packaged, which is exactly what the binary's gitinfo is not - so a `BUILD.txt` match is
    /// still the release, and still lists its platforms.
    #[test]
    fn a_build_stamp_match_is_still_the_release() {
        let kind = verdict_kind(&nothing_outstanding(), InstalledFrom::Build, REL, REL);
        assert_eq!(kind, "uptodate");
        assert_eq!(who_can_join(kind, REL, REL, &live_rows()), "Windows, macOS, Linux");
    }

    // -----------------------------------------------------------------------------------------
    // One identity, from the right evidence (Codex review of the release, 18/09/2026)
    // -----------------------------------------------------------------------------------------

    /// This file's production source - everything above the first test module - reduced to one
    /// function's body without its comments. For the three properties below that are WIRING rather
    /// than arithmetic: which function a builder calls. A pure test cannot see a call site.
    fn production_body(sig: &str) -> String {
        let src = include_str!("cmd_release.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod applicable_tests").expect("the first test module is gone")];
        let at = prod.find(sig).unwrap_or_else(|| panic!("`{sig}` is gone from cmd_release.rs"));
        let end = prod[at..].find("\n}\n").map_or(prod.len(), |e| at + e);
        prod[at..end]
            .lines()
            .map(str::trim)
            .filter(|l| !l.starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    fn item(platform: &str, name: &str) -> gxcore::InstalledItem {
        gxcore::InstalledItem { platform: platform.into(), name: name.into(), hash: "H".into() }
    }

    fn record_of(items: Vec<gxcore::InstalledItem>) -> gxcore::InstallState {
        gxcore::InstallState { ordinal: 1789427076, items }
    }

    /// What `self_update.rs` writes on macOS - including when the launcher was already current.
    fn launcher_only_record() -> gxcore::InstallState {
        record_of(vec![item("macos", "GeneralsXLauncher-macos-fc33f7efd.tar.gz")])
    }

    /// F6. The launcher's artifact name carries a commit too - the LAUNCHER's - and the first
    /// commit-bearing row used to become the engine build on the home screen.
    #[test]
    fn a_launcher_row_never_names_the_engine_build() {
        let st = record_of(vec![
            item("macos", "GeneralsXLauncher-macos-fc33f7efd.tar.gz"),
            item("win32", "GeneralsX-win64-8ff29efa7.zip"),
        ]);
        assert_eq!(
            commit_from_record(&st).as_deref(),
            Some("8ff29efa7"),
            "the launcher's commit was taken as the engine's"
        );
        assert_eq!(commit_from_record(&launcher_only_record()), None);
    }

    /// F6. A launcher-only record used to return `Record` before the engine binary was read, so a
    /// development engine beside a current launcher showed the launcher's commit as its build.
    #[test]
    fn a_launcher_only_record_leaves_the_engine_to_the_binary() {
        let id = identify(
            "macos",
            Some(&launcher_only_record()),
            String::new(),
            || Some(at_bin(stamp(DEV, "2026-09-15T20:48:29+03:00"))),
            |_| None,
            || Some(REL.into()),
            || None,
        );
        assert_eq!(id.from, InstalledFrom::Binary, "a launcher-only record was taken as engine evidence");
        assert_eq!(id.commit, "3fd38c2da", "the launcher's commit stood in for the engine build");
        assert_eq!(id.binary.expect("binary evidence").recorded, REL);

        // No binary either: nothing identifies the engine, and the launcher's commit is not
        // offered in its place.
        let id = identify("macos", Some(&launcher_only_record()), String::new(), || None, |_| None, || None, || None);
        assert_eq!(id.from, InstalledFrom::Nothing);
        assert_eq!(id.commit, "");
    }

    /// The release path, unchanged by F6: a Windows record (engine and tier rows) and `BUILD.txt`
    /// still answer on their own, and the binary, the engine record and the changelog are never
    /// even asked for. An EMPTY record - what an unreadable one parses to - also stays `Record` on
    /// Windows, and only there.
    #[test]
    fn an_engine_record_or_build_stamp_is_answered_without_the_binary() {
        let never_stamp = || -> Option<(std::path::PathBuf, gxio::enginestamp::EngineStamp)> { panic!("the binary was read") };
        let never_vouch = |_: &std::path::Path| -> Option<String> { panic!("the launcher's install was read") };
        let never_str = || -> Option<String> { panic!("the engine record was read") };
        let never_list = || -> Option<Vec<String>> { panic!("a changelog was asked for") };

        let win = record_of(vec![item("win32", "gxrt_d983ed3a.zip"), item("win32", "GeneralsX-win64-8ff29efa7.zip")]);
        let id = identify("win32", Some(&win), String::new(), never_stamp, never_vouch, never_str, never_list);
        assert_eq!((id.from, id.commit.as_str()), (InstalledFrom::Record, "8ff29efa7"));

        let id = identify("win32", None, "9beec5717".into(), never_stamp, never_vouch, never_str, never_list);
        assert_eq!((id.from, id.commit.as_str()), (InstalledFrom::Build, "9beec5717"));

        let id = identify("win32", Some(&record_of(Vec::new())), String::new(), never_stamp, never_vouch, never_str, never_list);
        assert_eq!(id.from, InstalledFrom::Record);

        // Off Windows the same empty record identifies nothing: the binary is asked, as with none.
        for platform in ["macos", "linux"] {
            let id = identify(platform, Some(&record_of(Vec::new())), String::new(), || Some(at_bin(stamp(DEV, ""))), |_| None, || None, || None);
            assert_eq!((id.from, id.commit.as_str()), (InstalledFrom::Binary, "3fd38c2da"), "{platform}");
        }
    }

    /// A launcher-only record's arithmetic is REAL - its launcher row carries a hash - so what it
    /// says would download still stands. Only its "up to date", which is about the launcher, may
    /// not speak for the engine.
    #[test]
    fn a_launcher_only_record_still_offers_the_launcher() {
        let launcher_due = gxcore::Verdict::Update(live_manifest().artifacts);
        assert_eq!(verdict_kind(&launcher_due, InstalledFrom::Binary, "3fd38c2da", REL), "update");
        assert_eq!(verdict_kind(&launcher_due, InstalledFrom::Nothing, "", REL), "update");
        let ahead = gxcore::Verdict::OlderThanInstalled { manifest: 1, installed: 2 };
        assert_eq!(verdict_kind(&ahead, InstalledFrom::Binary, "3fd38c2da", REL), "older");
        for commit in [REL, "3fd38c2da"] {
            assert_eq!(
                verdict_kind(&gxcore::Verdict::UpToDate, InstalledFrom::Binary, commit, REL),
                "unmanaged",
                "the launcher being current was reported as the engine being up to date"
            );
        }
    }

    /// F8. The engine record is shown ("the launcher recorded installing X here"), so it has to be
    /// read through `installed_sha_if_still_installed()`: the raw `installed_sha()` outlives the
    /// bundle it names, and the sentence would then name a build that is no longer anywhere.
    #[test]
    fn the_engine_record_is_read_through_the_checked_door() {
        let body = production_body("fn installed_identity(");
        assert!(
            body.contains("gxio::engine::installed_sha_if_still_installed"),
            "installed_identity does not read the checked engine record:\n{body}"
        );
        assert_eq!(
            body.matches("installed_sha").count(),
            body.matches("installed_sha_if_still_installed").count(),
            "the raw installed_sha() is read for display:\n{body}"
        );
    }

    /// F9. "Development build" is an absence, and only a changelog this launcher FETCHED lately -
    /// and not cut short at the cap - can show one. It used to be read off the CACHED FILE,
    /// whatever its age.
    #[test]
    fn only_a_fresh_complete_fetched_changelog_can_show_an_absence() {
        let s = std::time::Duration::from_secs;
        let one = || vec![REL.to_string()];
        assert_eq!(release_list_for_absence(Some((s(3), one()))), Some(one()));
        assert_eq!(release_list_for_absence(None), None, "no fetch in this process, yet a list");
        assert_eq!(
            release_list_for_absence(Some((FETCHED_FRESH_FOR + s(1), one()))),
            None,
            "a changelog fetched too long ago was trusted"
        );
        let capped: Vec<String> = (0..rel::CHANGELOG_MAX_RELEASES).map(|i| format!("{i:09x}")).collect();
        assert_eq!(release_list_for_absence(Some((s(3), capped))), None, "a list at the cap was trusted");

        // AND THE WIRING: the verdict reads the fetched memo, never the cached file, and only the
        // changelog's own network fetch fills that memo.
        let body = production_body("fn installed_identity(") + &production_body("fn build_release_state(");
        assert!(
            !body.contains("CHANGELOG_CACHE") && !body.contains("read_cached"),
            "the verdict reads the cached changelog file:\n{body}"
        );
        assert!(
            production_body("fn build_release_state(").contains("release_list_for_absence(fetched_releases())"),
            "build_release_state does not decide the release list by this rule"
        );
        assert!(production_body("fn build_changelog(").contains("*g = Some((std::time::Instant::now(), commits));"));
    }

    /// P2, SECOND REVIEW. The first F9 fix fetched the changelog inside `release_check`, serially,
    /// BEFORE the manifest - with the CDN unreachable that is curl's 8 s spent before the manifest
    /// is even asked for, and a second copy of the request `changelog_check` makes beside it.
    #[test]
    fn the_verdict_never_waits_on_the_changelog() {
        for f in ["fn build_release_state(", "fn installed_identity(", "fn release_list_for_absence("] {
            let body = production_body(f);
            assert!(
                !body.contains("fetch_https") && !body.contains("CHANGELOG_URL"),
                "{f} fetches the changelog on the verdict's path:\n{body}"
            );
        }
    }

    /// THE OWNER'S DECISION (after three review rounds each found a different leak in trying to
    /// approximate this instead): the disk-only path (`release_state`, `allow_network: false`)
    /// does not synthesize a `win_plan`-shaped verdict AT ALL - not by spawning the updater
    /// (round 1's fix: spawning here can block up to 60s behind the updater's own HTTPS
    /// bootstrap), and not by reusing or keying a past answer (rounds 2 and 3's fixes, both since
    /// removed). `None` sends `reconcile_plan` straight to `decide`'s own guess, already labelled
    /// `total_estimated` on Windows. Static, because `updater_check_plan` reaches the real
    /// filesystem and process table - this pins the call site's SHAPE, which is all a unit test
    /// can drive here.
    #[test]
    fn the_disk_only_path_never_synthesizes_a_plan() {
        let body = production_body("fn build_release_state(");
        assert!(
            body.contains("win_plan: if allow_network { updater_check_plan() } else { None }"),
            "the disk-only path is spawning, reusing or keying a plan again:\n{body}"
        );
    }

    // -----------------------------------------------------------------------------------------
    // The launcher's own install as evidence (P1, second review, 18/09/2026)
    // -----------------------------------------------------------------------------------------

    /// A CLEAN macOS LAUNCHER INSTALL IS UP TO DATE. `efd0a7242` made every binary-sourced match
    /// `unmanaged`, and a macOS release install has no `BUILD.txt` - so the release itself, installed
    /// by this launcher, read "Built from the release commit". The launcher's install vouching for
    /// this exact binary (`gxio::engine::launcher_vouches_for`, tested there against real bundles)
    /// is the evidence that puts it back.
    #[test]
    fn a_binary_the_launchers_install_vouches_for_is_the_release() {
        let id = identify(
            "macos",
            None,
            String::new(),
            || Some(at_bin(stamp(&format!("{REL}f36cb66f9ebfd7feb4fa3ca1a7934ab"), "2026-09-15T02:04:36+03:00"))),
            |_| Some(REL.into()),
            || panic!("the engine record is only read to report a disagreement"),
            || panic!("a vouched release needs no changelog"),
        );
        assert_eq!((id.from, id.commit.as_str()), (InstalledFrom::Installer, REL));
        let kind = verdict_kind(&nothing_outstanding(), id.from, &id.commit, REL);
        assert_eq!(kind, "uptodate", "a clean launcher install is not up to date");
        assert_eq!(who_can_join(kind, &id.commit, REL, &live_rows()), "Windows, macOS, Linux");
        // The Changelogs screen marks it INSTALLED, not SAME COMMIT.
        let c = changelog_dto(vec![rel_entry(1789427076, REL)], "network", &id, 0);
        assert_eq!((c.installed_from.as_str(), c.entries[0].installed), ("installer", true));

        // The launcher installed an OLDER release, and a newer one is published: that release is
        // named as what it is, and it is not up to date.
        let id = identify("macos", None, String::new(), || Some(at_bin(stamp(DEV, ""))), |_| Some("3fd38c2da".into()), || None, || None);
        assert_eq!(id.from, InstalledFrom::Installer);
        let kind = verdict_kind(&nothing_outstanding(), id.from, &id.commit, REL);
        assert_eq!(kind, "unmanaged");
        assert_eq!(
            unmanaged_detail(REL, &id),
            "This install is release 3fd38c2da, not the published release 8ff29efa7."
        );
    }

    /// A scratch install folder holding a packager's `BUILD.txt` for `commit`.
    fn folder_with_build_txt(tag: &str, commit: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("gxl-build-{tag}-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join("BUILD.txt"), format!("GeneralsX - Linux build\nbuilt from commit : {commit}\n")).unwrap();
        d
    }

    const REL40: &str = "8ff29efa7f36cb66f9ebfd7feb4fa3ca1a7934ab";

    /// macOS IS NEVER DECIDED FROM A BUILD.txt. A macOS release is identified by the launcher's own
    /// install vouching for the exact executable; a `BUILD.txt` - found anywhere - would win at
    /// `identify` before the vouch is asked, and a binary copied in by hand beside one would pass
    /// for the release again.
    #[test]
    fn macos_is_never_decided_from_a_build_txt() {
        let d = folder_with_build_txt("mac", REL40);
        let beside = || rel::parse_build_stamp(&format!("built from commit : {REL40}\n"));
        assert_eq!(
            rel::engine_build_stamp_on("macos", || Some(d.clone()), beside),
            None,
            "macOS was decided from a BUILD.txt"
        );
        let _ = std::fs::remove_dir_all(&d);
        // And every reader goes through that rule, never straight to the install directories.
        for f in ["fn installed_identity(", "pub fn about_info("] {
            let body = production_body(f);
            assert!(
                body.contains("engine_build_stamp()") && !body.contains("find_build_stamp"),
                "{f} reads BUILD.txt around the per-platform rule:\n{body}"
            );
        }
    }

    /// LINUX READS THE BUILD.txt BESIDE THE ENGINE IT RUNS - the folder `discover()` chose - and
    /// nowhere else, so the stamp and the engine it describes cannot come from two folders.
    #[test]
    fn linux_reads_build_txt_from_the_discovered_engine_folder_only() {
        let d = folder_with_build_txt("linux", REL40);
        let s = rel::engine_build_stamp_on("linux", || Some(d.clone()), || None).expect("the discovered folder's BUILD.txt");
        assert_eq!(rel::short_commit(&s.commit), REL);
        let elsewhere = || rel::parse_build_stamp("built from commit : 57d14a20b\n");
        assert_eq!(
            rel::engine_build_stamp_on("linux", || None, elsewhere),
            None,
            "a BUILD.txt outside the discovered engine folder was read"
        );
        let _ = std::fs::remove_dir_all(&d);
    }

    /// Windows is unchanged: the install directories, as before.
    #[test]
    fn windows_still_reads_build_txt_from_the_install_dirs() {
        let s = rel::engine_build_stamp_on("win32", || panic!("discovery is not asked on Windows"), || {
            rel::parse_build_stamp("built from commit : 8ff29efa7\n")
        });
        assert_eq!(s.map(|s| s.commit).as_deref(), Some("8ff29efa7"));
    }

    /// The payloads agree with the reader: the Linux bundler writes a BUILD.txt from the commit
    /// EMBEDDED in the binary it packs, and the macOS bundler writes none.
    #[test]
    fn only_the_linux_payload_ships_a_build_txt() {
        let linux = include_str!("../../../build/linux/bundle-linux-zh.sh");
        assert!(linux.contains("gx_embedded_commit \"${BUNDLE_DIR}/GeneralsXZH\""), "the Linux BUILD.txt is not read from the packed binary");
        assert!(linux.contains("> \"${BUNDLE_DIR}/BUILD.txt\""), "the Linux payload ships no BUILD.txt");
        assert!(!linux.contains("git rev-parse"), "the Linux BUILD.txt may not come from git HEAD");
        // A tarball short of mandatory archives says "not a release" in OVERLAY-MISSING.txt, and
        // must not also ship the packager's statement that it is one.
        assert!(
            linux.contains("GX_BUNDLE_NOT_A_RELEASE=1\n") && linux.contains("if [[ \"${GX_BUNDLE_NOT_A_RELEASE:-0}\" == \"1\" ]]; then"),
            "an incomplete tarball still ships a BUILD.txt"
        );
        let mac = include_str!("../../../build/macos/bundle-macos-zh.sh");
        assert!(!mac.contains("BUILD.txt"), "the macOS payload must not ship a BUILD.txt");
    }

    /// EVERY smoke-test arm is not a release - run, not read: the script's own overlay block under
    /// bash, once short of a mandatory archive and once short of an optional one only. The optional
    /// arm stamped "not a release" in OVERLAY-MISSING.txt and still shipped a BUILD.txt that told
    /// the launcher it was one (Codex, fifth review, P3).
    #[test]
    fn every_smoke_test_tarball_skips_its_build_txt() {
        let script = include_str!("../../../build/linux/bundle-linux-zh.sh");
        let head = "if [[ ${#gx_missing_required[@]} -gt 0 || ${#gx_missing_optional[@]} -gt 0 ]]; then";
        let at = script.find(head).expect("the overlay block is gone");
        let block = &script[at..at + script[at..].find("\nfi\n").expect("the overlay block closes") + 4];
        for (required, optional) in [("(!Required.big)", "()"), ("()", "(!Optional.big)")] {
            let dir = std::env::temp_dir().join(format!("gx-smoke-{}-{}", std::process::id(), required.len()));
            std::fs::create_dir_all(dir.join("stage")).unwrap();
            let run = format!(
                "GX_BUNDLE_ALLOW_NO_OVERLAY=1\nBUNDLE_DIR='{d}'\nGX_PATCH_STAGE='{d}/stage'\n\
                 gx_missing_required={required}\ngx_missing_optional={optional}\n{block}\
                 echo \"flag=${{GX_BUNDLE_NOT_A_RELEASE:-0}}\"\n",
                d = dir.display()
            );
            let out = std::process::Command::new("bash").arg("-c").arg(&run).output().expect("bash runs");
            let stdout = String::from_utf8_lossy(&out.stdout);
            let stamped = dir.join("OVERLAY-MISSING.txt").is_file();
            let _ = std::fs::remove_dir_all(&dir);
            assert!(out.status.success() && stamped, "the block did not run as a smoke test: {stdout}");
            assert!(stdout.contains("flag=1"), "missing {required} {optional}: a smoke test still ships a BUILD.txt");
        }
    }

    /// And NOTHING ELSE gets there. No vouch (a hand deploy, the owner's MacBook) or a vouch for a
    /// different build than the binary names, and the binary is only its commit.
    #[test]
    fn a_binary_the_launcher_does_not_vouch_for_is_not_the_release() {
        let full = format!("{REL}f36cb66f9ebfd7feb4fa3ca1a7934ab");
        for vouched in [None, Some("57d14a20b")] {
            let id = identify("macos", None, String::new(), || Some(at_bin(stamp(&full, ""))), |_| vouched.map(String::from), || None, || None);
            assert_eq!(id.from, InstalledFrom::Binary, "{vouched:?}");
            assert_eq!(verdict_kind(&nothing_outstanding(), id.from, &id.commit, REL), "unmanaged", "{vouched:?}");
        }
    }

    /// The wiring: the vouch production uses is the launcher's own install check.
    #[test]
    fn the_identity_asks_the_launchers_own_install() {
        let body = production_body("fn installed_identity(");
        assert!(
            body.contains("gxio::engine::launcher_vouches_for"),
            "installed_identity does not ask the launcher's own install:\n{body}"
        );
    }

    /// F10. The Changelogs screen used to read the record and `BUILD.txt` and never the engine, so
    /// it could mark the launcher's release (by the record's ordinal) or nothing at all, while the
    /// home screen named the engine's own commit. Both now read one identity.
    #[test]
    fn the_changelog_marks_the_build_the_home_screen_names() {
        let entries = vec![rel_entry(1789427076, "8ff29efa7"), rel_entry(1789419248, "3fd38c2da")];
        let marked = |c: &ChangelogDto| -> Vec<String> {
            c.entries.iter().filter(|e| e.installed).map(|e| e.commit.clone()).collect()
        };

        // A launcher-only record at the newest release's ordinal, and an engine built at 3fd38c2da.
        let id = identify("macos", Some(&launcher_only_record()), String::new(), || Some(at_bin(stamp(DEV, ""))), |_| None, || None, || None);
        let c = changelog_dto(entries.clone(), "cache", &id, 1789427076);
        assert_eq!(marked(&c), ["3fd38c2da"], "the changelog marked a different build from the home screen");
        assert_eq!((c.installed_commit.as_str(), c.installed_from.as_str()), ("3fd38c2da", "binary"));
        assert_eq!(c.installed_ordinal, 0, "the launcher's ordinal was reported as the engine's");

        // An engine record still decides by its ordinal, exactly as before.
        let id = identity_from(true, "8ff29efa7".into(), None, None, None);
        let c = changelog_dto(entries, "cache", &id, 1789419248);
        assert_eq!(marked(&c), ["3fd38c2da"]);
        assert_eq!(c.installed_from, "record");

        for f in ["fn build_changelog(", "fn build_release_state("] {
            assert!(production_body(f).contains("installed_identity("), "{f} does not read the one identity");
        }
    }

    /// Every arm is ONE sentence in the house voice - the rule the retired test above holds for the
    /// `none` arm, extended to the three arms this change added.
    #[test]
    fn every_unmanaged_sentence_is_one_sentence() {
        let rs = vec![REL.to_string()];
        let older = vec![REL.to_string(), "3fd38c2da".to_string()];
        let cases = [
            identity_from(false, String::new(), Some(stamp(REL, "")), None, Some(&rs)),
            identity_from(false, String::new(), Some(stamp(REL, "")), Some("57d14a20b".into()), None),
            identity_from(false, String::new(), Some(stamp(DEV, "")), None, Some(&older)),
            identity_from(false, String::new(), Some(stamp(DEV, "")), Some(REL.into()), Some(&rs)),
            identity_from(false, String::new(), Some(stamp(DEV, "")), None, Some(&rs)),
            identity_from(false, String::new(), Some(stamp(DEV, "")), None, None),
            identity_from(false, String::new(), None, None, None),
        ];
        for id in &cases {
            let d = unmanaged_detail(REL, id);
            assert_eq!(d.matches('.').count(), 1, "one sentence: {d}");
            assert!(!d.contains('!'), "{d}");
            assert!(!d.to_lowercase().contains("cannot"), "{d}");
            assert!(!d.to_lowercase().contains("up to date"), "{d}");
            assert!(d.contains(REL), "the published release is always named: {d}");
        }
    }

    // -----------------------------------------------------------------------------------------
    // "Players can join" (18/09/2026)
    // -----------------------------------------------------------------------------------------

    fn row(platform: &str, name: &str) -> gxcore::Artifact {
        gxcore::Artifact { platform: platform.into(), name: name.into(), hash: String::new(), size: 1 }
    }

    /// The live manifest's shape on 18/09/2026: three engine rows at 8ff29efa7, two launcher rows
    /// at their own commit, and hash-named tier files with no commit at all.
    fn live_rows() -> Vec<gxcore::Artifact> {
        vec![
            row("win32", "GeneralsX-win64-8ff29efa7.zip"),
            row("win32", "gxrt_d983ed3a.zip"),
            row("any", "gxreq_e2305004.zip"),
            row("macos", "GeneralsXZH-macos-arm64-8ff29efa7.zip"),
            row("linux", "GeneralsXZH-linux-x86_64-8ff29efa7.tar.gz"),
            row("macos", "GeneralsXLauncher-macos-fc33f7efd.tar.gz"),
            row("linux", "GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage"),
        ]
    }

    #[test]
    fn the_published_release_lists_the_platforms_its_manifest_publishes() {
        assert_eq!(who_can_join("uptodate", "8ff29efa7", "8ff29efa7", &live_rows()), "Windows, macOS, Linux");
    }

    #[test]
    fn a_platform_with_no_engine_at_this_commit_is_not_listed() {
        // Drop the Linux engine row; the Linux LAUNCHER row stays and must not count.
        let rows: Vec<_> = live_rows().into_iter().filter(|a| !a.name.starts_with("GeneralsXZH-linux")).collect();
        assert_eq!(who_can_join("uptodate", "8ff29efa7", "8ff29efa7", &rows), "Windows, macOS");
        // An engine row at a DIFFERENT commit is not this release's engine.
        let rows = vec![row("win32", "GeneralsX-win64-57d14a20b.zip"), row("macos", "GeneralsXZH-macos-arm64-8ff29efa7.zip")];
        assert_eq!(who_can_join("uptodate", "8ff29efa7", "8ff29efa7", &rows), "macOS");
    }

    /// THE OWNER'S MAC. A development build names no platforms - and does NOT say "same build only",
    /// which the measured shared source digest would make false.
    #[test]
    fn any_other_known_build_gets_the_engines_own_rule() {
        for kind in ["unmanaged", "update", "older", "unknown"] {
            assert_eq!(who_can_join(kind, "3fd38c2da", "8ff29efa7", &live_rows()), "matching builds only", "{kind}");
        }
        // uptodate on a DIFFERENT commit (a manifest with nothing for this platform) is not the release.
        assert_eq!(who_can_join("uptodate", "3fd38c2da", "8ff29efa7", &live_rows()), "matching builds only");
        // Current, but no nameable engine row at all: the rule, never an empty list.
        assert_eq!(who_can_join("uptodate", "8ff29efa7", "8ff29efa7", &[row("any", "gxreq_e2305004.zip")]),
            "matching builds only");
    }

    #[test]
    fn an_unknown_build_gets_no_row_at_all() {
        for kind in ["unmanaged", "uptodate", "unknown"] {
            assert_eq!(who_can_join(kind, "", "8ff29efa7", &live_rows()), "", "{kind}");
        }
    }

    #[test]
    fn the_row_never_carries_the_old_sample() {
        for (kind, inst) in [("uptodate", "8ff29efa7"), ("unmanaged", "3fd38c2da"), ("unknown", ""), ("update", "57d14a20b")] {
            let j = who_can_join(kind, inst, "8ff29efa7", &live_rows());
            assert!(!j.to_lowercase().contains("any platform"), "{kind}: {j}");
        }
    }
}

// =============================================================================================
// THE TRUTH TABLE (fifth review, 18/09/2026)
// =============================================================================================
//
// The reviewer's eight rows (R1-R8), Linux with no record (L), Windows with a full record (W), the
// one launcher rule (U) and the words (H): every install state the update screens have been wrong
// about, run through the composition that ships (`identify` -> `release_view`: `outstanding` ->
// `verdict_kind` -> `payload` -> `headline`) against the REAL signed document - the one served on 18/09/2026, verified with the embedded key
// exactly as the launcher verifies it. Each row asserts the HEADLINE a player reads and the
// DOWNLOAD LIST under it. One test, so a regression names every row it breaks at once.
#[cfg(test)]
mod truth_table {
    use super::*;
    use crate::self_update::OwnBuild;

    /// The live document, 18/09/2026: engine 8ff29efa7 on every platform, launcher fc33f7efd on
    /// macOS and Linux (Windows carries its launcher inside gxrt_), ordinal 1789427076. Byte-identical
    /// to the gxmanifest.txt the owner's Windows client cached, and verified below with the key.
    const LIVE: &str = "2|1789427076|0|j_Ke-n82|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-8ff29efa7.zip,4urEs9GQ1BVN5Bj6tdhiaQ,4520651;win32,gxrt_d983ed3a.zip,_-eaMN-oL7GYQf0l0GZ5FA,7411630;win32,gxopt_8ea4359c.zip,3Y_ig6ETpfyrzuaVG3JHzw,20919873;any,gxreq_e2305004.zip,12RkiEKE1eTUIjb-V61Ogg,36625184;macos,GeneralsXZH-macos-arm64-8ff29efa7.zip,rn14jenG62V1gM74GWTjKg,89690086;linux,GeneralsXZH-linux-x86_64-8ff29efa7.tar.gz,Pa-WZsJIJuCrsB2MZHGheA,235091200;macos,GeneralsXLauncher-macos-fc33f7efd.tar.gz,4Ux8cIeR8L1SaZJUFV7uBw,8238353;linux,GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage,ZROMm_niexIw9sbWMrqCow,79686136|W7Ojo7KbI8W3hGtyYnrcGA90r8Ztc-PvE6qKvqVUXI3KAi5mYrD2ycnBkSLWj1z0PnYMpSgFX4WkbVQkAs1nDA";

    fn live() -> gxcore::Manifest {
        gxcore::parse(&gxcore::verify(LIVE).expect("the live document verifies")).expect("and parses")
    }

    /// A LAUNCHER-ONLY RELEASE, as publish-bootstrap.sh allows one (its :376-403): gx-manifest.py
    /// signs the commit it runs at - here 4b38914f1, a launcher commit - with that commit's time as
    /// the ordinal, and new launcher rows, while every engine row still names 8ff29efa7. Built from
    /// the real document with exactly those fields changed; the key that would sign it is not in
    /// this repository, so the test takes the parsed struct past `verify`.
    fn launcher_only() -> gxcore::Manifest {
        let mut m = live();
        m.sha = "SziRTxmY".into(); // 4b38914f1998, base64url - gx-manifest.py's own encoding
        m.ordinal = 1_789_739_311; // git show -s --format=%ct 4b38914f1
        for a in m.artifacts.iter_mut().filter(|a| a.name.starts_with("GeneralsXLauncher-")) {
            a.name = a.name.replace("fc33f7efd", "4b38914f1");
            a.hash = "NEWLAUNCHERHASH0000000".into();
        }
        m
    }
    /// `m` with this platform's ENGINE row taken out - a document that publishes no engine here.
    fn no_engine_for(platform: &str, mut m: gxcore::Manifest) -> gxcore::Manifest {
        m.artifacts.retain(|a| !(a.platform == platform && !a.name.starts_with("GeneralsXLauncher-") && a.name.contains('-')));
        m
    }
    fn launcher_only_no_engines() -> gxcore::Manifest {
        no_engine_for("linux", no_engine_for("macos", launcher_only()))
    }
    const MAC_LAUNCHER_ONLY: OwnBuild<'static> = OwnBuild { commit9: "4b38914f1", commit_epoch: Some(1_789_739_311) };
    const LINUX_LAUNCHER_ONLY: OwnBuild<'static> = OwnBuild { commit9: "4b38914f1", commit_epoch: None };

    /// The owner's Windows install record as read over SSH on 18/09/2026, header and all. That
    /// client IS this launcher - a Tauri build of gxlauncher dated 25/08/2026, a day before fc33f7efd - whose
    /// Update runs GeneralsXUpdate.exe - so this is the state its screen is judged on.
    const OWNER_WINDOWS_RECORD: &str = "# GeneralsX installed state. Written by the launcher; delete to force a re-download.\nordinal 1789427076\nitem win32 GeneralsX-win64-8ff29efa7.zip 4urEs9GQ1BVN5Bj6tdhiaQ\nitem win32 gxrt_d983ed3a.zip _-eaMN-oL7GYQf0l0GZ5FA\nitem win32 gxopt_8ea4359c.zip 3Y_ig6ETpfyrzuaVG3JHzw\nitem any gxreq_e2305004.zip 12RkiEKE1eTUIjb-V61Ogg\n\n";

    const PUBLISHED: &str = "8ff29efa7f36cb66f9ebfd7feb4fa3ca1a7934ab";
    const MAC_ENGINE: &str = "GeneralsXZH-macos-arm64-8ff29efa7.zip";
    const DEV: &str = "3fd38c2daf2a77e1c9301dcd53e0d49613f2d397";
    const OLDER: &str = "57d14a20b0123456789abcdef0123456789abcde";

    // The running launcher. macOS packaged builds carry a commit time; no Linux AppImage does.
    const MAC_CURRENT: OwnBuild<'static> = OwnBuild { commit9: "fc33f7efd", commit_epoch: Some(1_789_400_000) };
    const MAC_OLDER: OwnBuild<'static> = OwnBuild { commit9: "57d14a20b", commit_epoch: Some(1_787_000_000) };
    const MAC_NEWER: OwnBuild<'static> = OwnBuild { commit9: "aaaaaaaaa", commit_epoch: Some(1_789_500_000) };
    const MAC_DEV: OwnBuild<'static> = OwnBuild { commit9: "044d50f68", commit_epoch: None };
    const LINUX_CURRENT: OwnBuild<'static> = OwnBuild { commit9: "fc33f7efd", commit_epoch: None };
    const LINUX_OLDER: OwnBuild<'static> = OwnBuild { commit9: "57d14a20b", commit_epoch: None };
    const WIN: OwnBuild<'static> = OwnBuild { commit9: "fc33f7efd", commit_epoch: None };

    /// A record row for a PUBLISHED artifact, carrying its real hash.
    fn has(m: &gxcore::Manifest, name: &str) -> gxcore::InstalledItem {
        let a = m.artifacts.iter().find(|a| a.name == name).unwrap_or_else(|| panic!("{name} is not published"));
        gxcore::InstalledItem { platform: a.platform.clone(), name: a.name.clone(), hash: a.hash.clone() }
    }
    /// A record row for an artifact of an EARLIER release.
    fn had(platform: &str, name: &str) -> gxcore::InstalledItem {
        gxcore::InstalledItem { platform: platform.into(), name: name.into(), hash: "EARLIERRELEASEHASH0000".into() }
    }
    fn record(ordinal: u64, items: Vec<gxcore::InstalledItem>) -> Option<gxcore::InstallState> {
        Some(gxcore::InstallState { ordinal, items })
    }

    struct Row {
        what: &'static str,
        platform: &'static str,
        record: Option<gxcore::InstallState>,
        /// `BUILD.txt`'s commit, as the per-platform rule reads it (never on macOS).
        build: &'static str,
        /// The commit in the engine binary, if one is readable.
        binary: Option<&'static str>,
        /// The launcher's own install vouches for that binary (macOS only).
        vouched: bool,
        /// The launcher's engine record, when it disagrees with the binary.
        recorded: Option<&'static str>,
        /// A changelog fetched moments ago.
        fetched: Option<Vec<String>>,
        own: OwnBuild<'static>,
        /// The signed document the row is judged against.
        doc: fn() -> gxcore::Manifest,
        /// The running launcher is a replaceable AppImage (`locate_target`) - every shipped Linux one.
        appimage: bool,
        /// The updater's own `--check` plan, on Windows. `None` = the updater could not be asked, so
        /// `decide`'s own guess stands (and is flagged an estimate); `Some` = authoritative, and it
        /// replaces the guess. Off Windows this is always `None`.
        win_plan: Option<gxio::release::UpdaterPlan>,
        /// The published ENGINE row an UPDATE press installs (macOS, `cmd_setup::engine_update_row`).
        engine: Option<&'static str>,
        headline: &'static str,
        downloads: Vec<&'static str>,
    }

    fn view(r: &Row) -> ReleaseStateDto {
        let m = (r.doc)();
        let identity = identify(
            r.platform,
            r.record.as_ref(),
            r.build.to_string(),
            || {
                r.binary.map(|c| {
                    let s = gxio::enginestamp::EngineStamp {
                        commit: c.into(),
                        short: rel::short_commit(c),
                        built: "2026-09-15T02:04:36+03:00".into(),
                    };
                    (std::path::PathBuf::from("/Applications/GeneralsXZH.app/Contents/Resources/bin/GeneralsXZH"), s)
                })
            },
            |_| (r.vouched).then(|| rel::short_commit(r.binary.expect("a vouch needs a binary"))),
            || r.recorded.map(String::from),
            || r.fetched.clone(),
        );
        release_view(
            Some(&m),
            "network",
            true,
            &Disk {
                platform: r.platform,
                record: r.record.as_ref(),
                identity: &identity,
                build_commit: r.build.to_string(),
                own: r.own,
                appimage: r.appimage,
                engine: r.engine.map(|n| m.artifacts.iter().find(|a| a.name == n).cloned().expect("a published engine row")),
                install_dir: String::new(),
                win_plan: r.win_plan.clone(),
            },
        )
    }

    fn rows() -> Vec<Row> {
        let m = live();
        let row = |what, platform, own, headline, downloads: Vec<&'static str>| Row {
            what,
            platform,
            record: None,
            build: "",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own,
            doc: live,
            appimage: platform == "linux",
            win_plan: None,
            engine: None,
            headline,
            downloads,
        };
        let mac_launcher = || has(&m, "GeneralsXLauncher-macos-fc33f7efd.tar.gz");
        let linux_launcher = || has(&m, "GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage");
        let windows_all = || {
            ["GeneralsX-win64-8ff29efa7.zip", "gxrt_d983ed3a.zip", "gxopt_8ea4359c.zip", "gxreq_e2305004.zip"]
                .iter()
                .map(|n| has(&m, n))
                .collect::<Vec<_>>()
        };
        let windows_previous = || {
            let mut v = windows_all();
            v[0] = had("win32", "GeneralsX-win64-79438b54f.zip");
            v
        };
        let every_windows_row = || vec!["gxreq_e2305004.zip", "gxopt_8ea4359c.zip", "gxrt_d983ed3a.zip", "GeneralsX-win64-8ff29efa7.zip"];
        let previous_mac_launcher = || record(1_787_214_478, vec![had("macos", "GeneralsXLauncher-macos-57d14a20b.tar.gz")]);
        let appimage = || vec!["GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage"];
        vec![
            // ---- THE REVIEWER'S EIGHT (fourth review of fa72c21c9), in its order -----------------
            Row { binary: Some(PUBLISHED), vouched: true, record: record(m.ordinal, vec![mac_launcher()]),
                ..row("R1 macOS clean launcher install of the release, launcher current", "macos", MAC_CURRENT, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true, record: previous_mac_launcher(),
                ..row("R2 the same, the launcher's own update deferred (previous launcher recorded and running)", "macos", MAC_OLDER, "Update available", vec!["GeneralsXLauncher-macos-fc33f7efd.tar.gz"]) },
            Row { binary: Some(DEV), recorded: Some("8ff29efa7"),
                ..row("R3 owner's MacBook: dev engine 3fd38c2da by hand, engine record 8ff29efa7 with no stamp, launcher NEWER than published", "macos", MAC_NEWER, "Not the published release", vec![]) },
            Row { build: "8ff29efa7", record: record(m.ordinal, vec![linux_launcher()]),
                ..row("R4 Linux tarball at the published commit (BUILD.txt), launcher current", "linux", LINUX_CURRENT, "Up to date", vec![]) },
            Row { build: "57d14a20b", record: record(m.ordinal, vec![linux_launcher()]),
                ..row("R5 Linux tarball at an OLDER commit, launcher current", "linux", LINUX_CURRENT, "Not the published release", vec![]) },
            Row { build: "8ff29efa7", record: record(1_787_214_478, vec![had("linux", "GeneralsXLauncher-linux-x86_64-57d14a20b.AppImage")]),
                ..row("R6 Linux previous AppImage recorded and running", "linux", LINUX_OLDER, "Update available", appimage()) },
            Row { build: "8ff29efa7", record: Some(gxcore::InstallState::parse(OWNER_WINDOWS_RECORD)),
                ..row("R7 Windows full record of the release: the owner's gxinstalled.txt, verbatim", "win32", WIN, "Up to date", vec![]) },
            // The reviewer expected "Not the published release" - fa72c21c9's answer, reached only
            // because the filter had removed every Windows row. On Windows, Update runs
            // GeneralsXUpdate.exe, which with no record fetches every row: this IS an update.
            Row { build: "57d14a20b",
                ..row("R8 Windows NO record, BUILD.txt older (an old share zip)", "win32", WIN, "Update available", every_windows_row()) },
            // ---- LINUX WITH NO RECORD - the normal Linux state (Codex P1 on fa72c21c9) ------------
            Row { build: "8ff29efa7",
                ..row("L1 Linux NO record, previous AppImage, tarball at the published commit", "linux", LINUX_OLDER, "Update available", appimage()) },
            Row { build: "3fd38c2da",
                ..row("L2 Linux NO record, previous AppImage, another engine", "linux", LINUX_OLDER, "Update available", appimage()) },
            Row { build: "8ff29efa7",
                ..row("L3 Linux NO record, current AppImage, tarball at the published commit", "linux", LINUX_CURRENT, "Up to date", vec![]) },
            // A launcher republished under the SAME commit-stamped name with corrected bytes: the
            // record of the pressed Update holds the old hash, so the new one is outstanding - as
            // `decide` treats a hash change (Codex, sixth round). R4 is the same row, hash current.
            Row { build: "8ff29efa7", record: record(m.ordinal, vec![had("linux", "GeneralsXLauncher-linux-x86_64-fc33f7efd.AppImage")]),
                ..row("L4 Linux AppImage republished at the running commit with a corrected hash", "linux", LINUX_CURRENT, "Update available", appimage()) },
            // A Linux build that is NOT running from an AppImage - `cargo run`, a hand-run `cargo
            // tauri build` - has no commit time either, but nothing can replace it: offering the
            // AppImage sent it to MainUpdate, whose Update can only fail (Codex, eleventh round).
            Row { build: "8ff29efa7", appimage: false,
                ..row("L5 Linux development launcher, not an AppImage, other commit", "linux", LINUX_OLDER, "Up to date", vec![]) },
            // ---- WINDOWS WITH A FULL RECORD, BEHIND (the pre-existing fc33f7efd filter) ------------
            Row { build: "79438b54f", record: record(1_789_419_248, windows_previous()),
                ..row("W1 Windows full record of the PREVIOUS release", "win32", WIN, "Update available", vec!["GeneralsX-win64-8ff29efa7.zip"]) },
            // No record is what the updater fetches everything for - deleting it is the documented
            // way to force a re-download - and BUILD.txt vouches for the engine tier only.
            // The engine is RE-FETCHED although BUILD.txt already names the published commit, because
            // no record proves the rest of the install. That is a reinstall of the current release,
            // NOT a newer build, so it must not say "Update available" over "You have X / New X" - the
            // lie the owner photographed.
            Row { build: "8ff29efa7",
                ..row("W2 Windows NO record, BUILD.txt at the published commit", "win32", WIN, "Reinstalling this release", every_windows_row()) },
            // GeneralsXUpdate.exe's state_load discards a record BUILD.txt contradicts, ordinal and
            // all, before plan_update reads anything - and then plans every row. This IS the owner's
            // case: a setup ran over the install, so the engine is re-fetched at its own commit.
            Row { build: "8ff29efa7", record: record(1_789_419_248, windows_previous()),
                ..row("W3 Windows record contradicted by BUILD.txt (a setup ran over it)", "win32", WIN, "Reinstalling this release", every_windows_row()) },
            Row { build: "57d14a20b", record: record(1_789_419_248, windows_previous()),
                ..row("W4 Windows record contradicted by an OLDER BUILD.txt", "win32", WIN, "Update available", every_windows_row()) },
            Row { build: "8ff29efa7", record: record(m.ordinal + 1, windows_all()),
                ..row("W5 Windows record NEWER than the document", "win32", WIN, "Ahead of what is published", vec![]) },
            Row { build: "8ff29efa7", record: record(m.ordinal + 1, windows_previous()),
                ..row("W6 Windows record NEWER than the document but contradicted by BUILD.txt", "win32", WIN, "Reinstalling this release", every_windows_row()) },
            // ---- ONE LAUNCHER RULE: the running binary, never the record's hash -------------------
            Row { binary: Some(PUBLISHED), vouched: true,
                ..row("U1 macOS clean install, no launcher record yet", "macos", MAC_CURRENT, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true,
                ..row("U2 macOS vouched engine, NO record, previous launcher running", "macos", MAC_OLDER, "Update available", vec!["GeneralsXLauncher-macos-fc33f7efd.tar.gz"]) },
            Row { binary: Some(PUBLISHED), vouched: true, record: previous_mac_launcher(),
                ..row("U3 stale launcher record beside a DEV launcher", "macos", MAC_DEV, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true, record: previous_mac_launcher(),
                ..row("U4 stale launcher record beside a NEWER launcher", "macos", MAC_NEWER, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true, record: previous_mac_launcher(),
                ..row("U5 stale launcher record, the running launcher IS the published one", "macos", MAC_CURRENT, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true, record: record(m.ordinal + 1, vec![mac_launcher()]),
                ..row("U6 macOS launcher record NEWER than the document", "macos", MAC_OLDER, "Ahead of what is published", vec![]) },
            // ---- a macOS/Linux gxinstalled.txt never names the ENGINE (Codex, fourth round) --------
            // The launcher writes only its own row there, so an unreadable record or one that lists
            // engine rows is not evidence of the engine; the binary answers, as with no record.
            Row { binary: Some(DEV), record: Some(gxcore::InstallState::parse("garbage\n")),
                ..row("X1 macOS unreadable record beside a hand-deployed dev engine", "macos", MAC_CURRENT, "Not the published release", vec![]) },
            Row { binary: Some(DEV), record: Some(gxcore::InstallState::parse(OWNER_WINDOWS_RECORD)),
                ..row("X2 macOS record listing ENGINE rows (a Windows record copied over)", "macos", MAC_CURRENT, "Not the published release", vec![]) },
            Row { build: "57d14a20b", record: Some(gxcore::InstallState::parse("")),
                ..row("X3 Linux empty record, older tarball", "linux", LINUX_CURRENT, "Not the published release", vec![]) },
            // ---- a LAUNCHER-ONLY release: the signed commit is the launcher's (Codex, seventh round)
            // The engine is judged against this platform's ENGINE row, never the signed HEAD commit,
            // or every install of the unchanged engine reads "Not the published release".
            Row { binary: Some(PUBLISHED), vouched: true, doc: launcher_only,
                ..row("O1 macOS vouched release engine, launcher-only release installed", "macos", MAC_LAUNCHER_ONLY, "Up to date", vec![]) },
            Row { build: "8ff29efa7", doc: launcher_only,
                ..row("O2 Linux release tarball, launcher-only release installed", "linux", LINUX_LAUNCHER_ONLY, "Up to date", vec![]) },
            Row { build: "8ff29efa7", doc: launcher_only,
                ..row("O3 Linux release tarball, launcher-only release NOT yet installed", "linux", LINUX_CURRENT, "Update available", vec!["GeneralsXLauncher-linux-x86_64-4b38914f1.AppImage"]) },
            Row { binary: Some(PUBLISHED), doc: launcher_only,
                ..row("O4 macOS binary at the ENGINE's release commit, not vouched", "macos", MAC_LAUNCHER_ONLY, "Built from the release commit", vec![]) },
            // A launcher-only document that publishes NO engine for the platform: nothing names the
            // published engine, so nothing is compared with it - what the build evidence names and
            // nothing outstanding is "Up to date"; a bare binary still is not (Codex, tenth round).
            Row { build: "8ff29efa7", doc: launcher_only_no_engines,
                ..row("O5 Linux release tarball, the document names no Linux engine", "linux", LINUX_LAUNCHER_ONLY, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), vouched: true, doc: launcher_only_no_engines,
                ..row("O6 macOS vouched engine, the document names no macOS engine", "macos", MAC_LAUNCHER_ONLY, "Up to date", vec![]) },
            Row { binary: Some(PUBLISHED), doc: launcher_only_no_engines,
                ..row("O7 macOS unvouched binary, the document names no macOS engine", "macos", MAC_LAUNCHER_ONLY, "Not the published release", vec![]) },
            // ---- the words, told apart by the evidence (`headline`) -------------------------------
            Row { binary: Some(DEV), recorded: Some("8ff29efa7"), fetched: Some(vec!["8ff29efa7".into(), "79438b54f".into()]),
                ..row("H1 R3 with a dev launcher and a changelog fetched moments ago", "macos", MAC_DEV, "Development build", vec![]) },
            Row { binary: Some(PUBLISHED),
                ..row("H2 macOS binary replaced by hand at the release commit", "macos", MAC_CURRENT, "Built from the release commit", vec![]) },
            Row { binary: Some(OLDER), vouched: true, record: record(m.ordinal, vec![mac_launcher()]),
                ..row("H3 macOS launcher-installed OLDER release, newer published", "macos", MAC_CURRENT, "Not the published release", vec![]) },
            // ---- the macOS ENGINE rides the verdict (28/09/2026), so UPDATE installs it as on Windows
            Row { binary: Some(OLDER), vouched: true, record: record(m.ordinal, vec![mac_launcher()]), engine: Some(MAC_ENGINE),
                ..row("E1 H3 with the newer engine outstanding (engine_update_row)", "macos", MAC_CURRENT, "Update available", vec![MAC_ENGINE]) },
            Row { binary: Some(OLDER), vouched: true, record: previous_mac_launcher(), engine: Some(MAC_ENGINE),
                ..row("E2 macOS engine AND launcher outstanding, biggest first", "macos", MAC_OLDER, "Update available", vec![MAC_ENGINE, "GeneralsXLauncher-macos-fc33f7efd.tar.gz"]) },
            // An engine this launcher did not install, at the published commit: the press re-fetches the
            // release to put one it can vouch for in place - never called a newer build.
            Row { binary: Some(PUBLISHED), engine: Some(MAC_ENGINE),
                ..row("E3 macOS unvouched engine at the release commit, engine offered", "macos", MAC_CURRENT, "Reinstalling this release", vec![MAC_ENGINE]) },
            // A document OLDER than this install installs nothing, the engine included.
            Row { binary: Some(OLDER), vouched: true, record: record(m.ordinal + 1, vec![mac_launcher()]), engine: Some(MAC_ENGINE),
                ..row("E4 macOS launcher record NEWER than the document, engine offered", "macos", MAC_CURRENT, "Ahead of what is published", vec![]) },
        ]
    }

    #[test]
    fn the_truth_table() {
        let mut wrong = Vec::new();
        for r in rows() {
            let v = view(&r);
            let got: Vec<String> = v.items.iter().map(|i| i.name.clone()).collect();
            if v.headline != r.headline || got != r.downloads {
                wrong.push(format!(
                    "row {}: got ({:?}, {:?}) [{} from {}], want ({:?}, {:?})",
                    r.what, v.headline, got, v.verdict, v.installed_from, r.headline, r.downloads
                ));
            }
            super::tests::release_state_invariants(&v);
            // Words that promise nothing will download never sit over a download list, and the
            // total is the list's own sum.
            assert_eq!(v.total_bytes, v.items.iter().map(|i| i.size).sum::<u64>(), "row {}", r.what);
            if v.verdict != "update" {
                assert!(v.items.is_empty(), "row {}: {} carries items", r.what, v.verdict);
            }
        }
        assert!(wrong.is_empty(), "the truth table disagrees:\n{}", wrong.join("\n"));
    }

    /// The engine row answers, per platform, whatever commit the document was signed at. With no
    /// engine row NOTHING does (Codex, tenth round): the signed commit is whatever HEAD was when
    /// the document was generated - the engine's for the live document, the launcher's for every
    /// document up to 11/09/2026 and for a launcher-only publish - and a launcher row's commit is
    /// the launcher's.
    #[test]
    fn the_published_engine_commit_is_the_engine_rows() {
        for platform in ["win32", "macos", "linux"] {
            assert_eq!(published_engine_commit(&launcher_only(), platform), "8ff29efa7", "{platform}");
        }
        let m = no_engine_for("macos", live());
        assert_eq!(published_engine_commit(&m, "macos"), "", "neither the signed commit nor the launcher row's fc33f7efd");
    }

    /// W3's record is one GeneralsXUpdate.exe discards, so the screen must not report it either:
    /// the build is what BUILD.txt names, with no "released" ordinal borrowed from the stale record.
    #[test]
    fn a_record_the_updater_discards_is_not_what_is_installed() {
        let w3 = rows().into_iter().find(|r| r.what.starts_with("W3 ")).expect("row W3");
        let v = view(&w3);
        assert_eq!((v.installed_from.as_str(), v.installed_ordinal), ("build", 0));
    }

    /// The updater's own `--check` plan (four tiers) as `parse_updater_plan` returns it.
    fn four_tier_plan() -> gxio::release::UpdaterPlan {
        gxio::release::parse_updater_plan(
            "plan: win32 GeneralsX-win64-8ff29efa7.zip H1 4530242\n\
             plan: win32 gxrt_d983ed3a.zip H2 7411630\n\
             plan: win32 gxopt_8ea4359c.zip H3 20919873\n\
             plan: any gxreq_e2305004.zip H4 36625184\n\
             plan-total: 4 69486929\n",
        )
        .expect("plan-total present")
    }

    /// DEFECT 1, PINNED. The owner's exact case: the record's engine is stale so the launcher's own
    /// `decide` would name ONLY the engine tier (4.3 MB), but the updater will fetch all four. When
    /// the updater's plan is in hand the estimate IS that plan - every tier, and its total - never
    /// one tier as the whole. Fails on revert: with `win_plan: None` the row reports 4.3 MB.
    #[test]
    fn the_windows_estimate_is_the_updaters_whole_plan_not_one_tier() {
        let m = live();
        // A record whose data tiers are current but whose engine is an older release - decide alone
        // would call only the engine outstanding.
        let stale = record(
            m.ordinal,
            vec![
                had("win32", "GeneralsX-win64-79438b54f.zip"),
                has(&m, "gxrt_d983ed3a.zip"),
                has(&m, "gxopt_8ea4359c.zip"),
                has(&m, "gxreq_e2305004.zip"),
            ],
        );
        let base = Row {
            engine: None,
            what: "estimate is the whole plan",
            platform: "win32",
            record: stale,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: Some(four_tier_plan()),
            headline: "",
            downloads: vec![],
        };

        // Without the updater's plan, and with BUILD.txt briefly unreadable (build ""), the launcher
        // fails OPEN: it trusts the stale record and names only the engine tier - the owner's exact
        // 4.3 MB understatement. The live test engine is 4520651 bytes.
        let guessed = view(&Row { win_plan: None, build: "", ..clone_row(&base) });
        assert_eq!(guessed.items.len(), 1, "the fail-open guess sees only the engine");
        assert_eq!(guessed.items[0].label, "engine");
        assert_eq!(guessed.total_bytes, 4_520_651, "one tier named as the whole");
        assert!(guessed.total_estimated, "and it is flagged an estimate, not a fact");

        // With it, the estimate is the whole download the press will perform.
        let v = view(&base);
        assert_eq!(v.total_bytes, 69_486_929, "every tier, the updater's own total");
        assert!(!v.total_estimated, "authoritative - not an estimate");
        assert_eq!(v.total_bytes, v.items.iter().map(|i| i.size).sum::<u64>());
        assert_eq!(v.items.len(), 4);
        assert_eq!(v.items[0].size, 36_625_184, "biggest first, like the download is felt");
    }

    /// DEFECT 2, PINNED. Installed engine commit equals the published one, yet the updater still
    /// re-fetches the engine (no trustworthy record). That is a reinstall of the SAME build, so the
    /// screen must not say "Update available" over "You have X / New X". Fails on revert: the old
    /// headline was "Update available".
    #[test]
    fn a_same_commit_engine_refetch_is_a_reinstall_not_an_update() {
        let v = view(&rows().into_iter().find(|r| r.what.starts_with("W2 ")).expect("W2"));
        assert_eq!(v.headline, "Reinstalling this release");
        assert_eq!(v.installed_commit, v.available_commit, "identical commit - no newer build");
        assert!(!v.installed_commit.is_empty());
        assert_eq!(v.verdict, "update", "a download IS pending, so it is not 'up to date'");
    }

    /// The updater reporting an EMPTY plan overrides a stale-record 'update' the launcher derived:
    /// nothing to fetch means nothing is claimed downloadable, at any commit.
    #[test]
    fn an_empty_updater_plan_downgrades_a_derived_update() {
        let m = live();
        let stale = record(m.ordinal, vec![had("win32", "GeneralsX-win64-79438b54f.zip")]);
        let empty = gxio::release::parse_updater_plan("plan-total: 0 0\n").expect("zero is an answer");
        let v = view(&Row {
            engine: None,
            what: "empty plan wins",
            platform: "win32",
            record: stale,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: Some(empty),
            headline: "",
            downloads: vec![],
        });
        assert_eq!(v.verdict, "uptodate");
        assert_eq!(v.total_bytes, 0);
        assert!(v.items.is_empty());
        assert!(!v.total_estimated, "an authoritative zero is not an estimate");
    }
    /// THE COMPLETED-UPDATE CASE (Codex, third round) IS FIXED FOR FREE by removing the cache
    /// rather than by anything that has to remember an apply happened. `decide`'s guess is a pure
    /// function of the CURRENT manifest and the CURRENT install record - `rel::read_install_record`
    /// is read fresh on every `build_release_state` call, with no memo - so once a successful
    /// apply rewrites `gxinstalled.txt`, the very next guess is computed against the NEW record
    /// and already agrees with it, with no win_plan around to override it back to "update". This
    /// pins that: a record whose items now match what a release install would look like guesses
    /// "uptodate" although the platform is win32 and no win_plan is ever involved on this path.
    #[test]
    fn the_guess_alone_reflects_a_completed_apply_because_the_record_is_read_fresh() {
        let m = live();
        let current = record(
            m.ordinal,
            vec![
                has(&m, "GeneralsX-win64-8ff29efa7.zip"),
                has(&m, "gxrt_d983ed3a.zip"),
                has(&m, "gxopt_8ea4359c.zip"),
                has(&m, "gxreq_e2305004.zip"),
            ],
        );
        let v = view(&Row {
            engine: None,
            what: "just applied",
            platform: "win32",
            record: current,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: None,
            headline: "",
            downloads: vec![],
        });
        assert_eq!(v.verdict, "uptodate", "the record now matches - no stale plan is left to say otherwise");
        assert!(v.items.is_empty());
    }


    /// A NON-EMPTY plan with total 0 is the C updater's "could not price at least one pending
    /// artifact" signal (`parse_updater_plan`'s "denominator unknown" case), not "nothing costs
    /// anything" - that authoritative zero only exists for EMPTY rows, above. Fails on revert: the
    /// old code returned `total_estimated: false` here, so the page showed "0.0 MB" while the row
    /// below it kept downloading real bytes.
    #[test]
    fn a_nonempty_plan_with_a_zero_total_is_unpriced_not_zero() {
        let m = live();
        let stale = record(m.ordinal, vec![had("win32", "GeneralsX-win64-79438b54f.zip")]);
        let unpriced = gxio::release::parse_updater_plan(
            "plan: win32 GeneralsX-win64-8ff29efa7.zip H1 0\n\
             plan-total: 1 0\n",
        )
        .expect("plan-total present");
        let v = view(&Row {
            engine: None,
            what: "unpriced plan",
            platform: "win32",
            record: stale,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: Some(unpriced),
            headline: "",
            downloads: vec![],
        });
        assert_eq!(v.verdict, "update", "the row is real - it just cannot be priced yet");
        assert_eq!(v.items.len(), 1);
        assert_eq!(v.total_bytes, 0);
        assert!(v.total_estimated, "0 bytes with a real row must read 'Calculating...', not '0.0 MB'");
    }

    /// A one-row plan for a mixed-plan test: real bytes for a real row plus one unpriced row.
    fn mixed_plan() -> gxio::release::UpdaterPlan {
        gxio::release::parse_updater_plan(
            "plan: win32 GeneralsX-win64-8ff29efa7.zip H1 4520651\n\
             plan: any gxreq_e2305004.zip H2 0\n\
             plan-total: 2 4520651\n",
        )
        .expect("plan-total present")
    }

    /// DEFECT, PINNED (Codex, second round). `plan_update` sums only the rows it CAN price, so a
    /// plan with one priced row and one unpriced row carries a NONZERO partial total -
    /// `print_plan` only forces the printed total to 0 when EVERY row is unpriced. Keying
    /// `unpriced` off `plan.total == 0` alone missed exactly this, the common case (most updates
    /// touch several tiers, and it takes only one still being sized to taint the total), and the
    /// launcher presented the partial sum as though it were the whole download.
    #[test]
    fn a_mixed_plan_with_one_unpriced_row_is_still_unpriced() {
        let m = live();
        let stale = record(m.ordinal, vec![had("win32", "GeneralsX-win64-79438b54f.zip")]);
        let v = view(&Row {
            engine: None,
            what: "mixed plan",
            platform: "win32",
            record: stale,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: Some(mixed_plan()),
            headline: "",
            downloads: vec![],
        });
        assert_eq!(v.verdict, "update");
        assert_eq!(v.items.len(), 2);
        assert_eq!(v.total_bytes, 4_520_651, "the partial sum the updater printed, not invented");
        assert!(v.total_estimated, "a nonzero PARTIAL total is still not the whole download");
    }

    /// DEFECT, PINNED (Codex, second round). `total_estimated` flipping to `true` is not enough on
    /// its own: `release_view` kept passing the raw zero straight to `describe`, so the SAME screen
    /// still read "0.0 MB to download ... [tier] 0.0 MB" in the sentence beside the number that had
    /// just been fixed to say "Calculating...". The unpriced state has to reach the sentence, and
    /// the per-item text for the specific row that is not yet sized, not just the numeric flag.
    #[test]
    fn an_unpriced_plans_detail_says_so_instead_of_a_false_zero() {
        let m = live();
        let stale = record(m.ordinal, vec![had("win32", "GeneralsX-win64-79438b54f.zip")]);
        let v = view(&Row {
            engine: None,
            what: "mixed plan",
            platform: "win32",
            record: stale,
            build: "8ff29efa7",
            binary: None,
            vouched: false,
            recorded: None,
            fetched: None,
            own: WIN,
            doc: live,
            appimage: false,
            win_plan: Some(mixed_plan()),
            headline: "",
            downloads: vec![],
        });
        assert!(v.detail.contains("still being worked out"), "{}", v.detail);
        assert!(v.detail.contains("size not yet known"), "{}", v.detail);
        assert!(!v.detail.contains("0.0 MB"), "the old false zero is back in the sentence:\n{}", v.detail);
    }

    fn clone_row(r: &Row) -> Row {
        Row {
            engine: None,
            what: r.what,
            platform: r.platform,
            record: r.record.clone(),
            build: r.build,
            binary: r.binary,
            vouched: r.vouched,
            recorded: r.recorded,
            fetched: r.fetched.clone(),
            own: r.own,
            doc: r.doc,
            appimage: r.appimage,
            win_plan: r.win_plan.clone(),
            headline: r.headline,
            downloads: r.downloads.clone(),
        }
    }

    /// "Development build" rests on the changelog `changelog_check` fetches BESIDE `release_check`,
    /// which reads the list before its own fetch - so the first check of a session could only say
    /// the weaker "Not the published release", and nothing asked again (Codex, fifth review, P2).
    /// MainWeb now asks once more after both settle, for that state only - `unmanaged` from the
    /// binary with the absence unknown, the one headline the list changes; an `unknown` verdict is
    /// "Could not check" whatever the list says, and re-asking there only swallowed a recovered
    /// answer (Codex, second round) - and repaints the headline and nothing else: the note line
    /// beside PLAY belongs to the engine-outcome block.
    #[test]
    fn mainweb_asks_again_once_the_changelog_has_landed() {
        let web = include_str!("../../../../design/launcher/screens/MainWeb.html");
        let at = web.find("function networkRefresh() {").expect("networkRefresh is gone");
        let body = &web[at..at + web[at..].find("\n  }\n").expect("networkRefresh closes")];
        let again = &body[body.find("Promise.all([releaseNet, clogNet])").expect("no re-check after the changelog")..];
        assert!(
            again.contains("s.verdict !== 'unmanaged' || s.installed_from !== 'binary' || s.installed_unpublished != null")
                // Only an answer about the SAME release and the SAME install may change the words
                // alone; anything else waits for the next check (Codex, tenth round).
                && again.contains("n.available_ordinal !== s.available_ordinal || n.available_commit !== s.available_commit")
                && again.contains("n.installed_commit !== s.installed_commit || n.installed_from !== s.installed_from")
                && again.contains("invoke('release_check')")
                && again.contains("statusLabel.textContent = statusText(lastState);")
                && !again.contains("gxSay")
                && !again.contains("paintRelease"),
            "the re-check is not narrowed to the headline:\n{again}"
        );
    }

    /// THE PRESSED UPDATE FOLLOWS THE SAME LAUNCHER RULE the table does. It used to ask `decide`
    /// against the install record, so a stale record could offer - and this path install - a
    /// launcher every screen called current, and a Linux AppImage with no record offered the same
    /// AppImage it was running.
    #[test]
    fn the_pressed_update_follows_the_one_launcher_rule() {
        let src = include_str!("cmd_release.rs");
        let at = src.find("pub fn release_apply(").expect("release_apply is gone");
        let body = &src[at..at + src[at..].find("\n}\n").expect("release_apply closes")];
        let mac = body.find("#[cfg(target_os = \"macos\")]").expect("the macOS arm");
        let linux = body.find("#[cfg(not(any(target_os = \"windows\", target_os = \"macos\")))]").expect("the Linux arm");
        let (mac, linux) = (&body[mac..linux], &body[linux..]);
        assert!(
            linux.contains("launcher_outstanding(&m, platform, crate::self_update::own_build(), Some(&installed), matches!(target, gxio::update::Target::AppImage(_)))")
                && !linux.contains("gxcore::decide("),
            "the pressed Update decides by a rule of its own:\n{linux}"
        );
        // macOS (28/09/2026): the same launcher rule, and the ENGINE by the rule the verdict lists it
        // by - so UPDATE installs exactly what the Update page said would download.
        assert!(
            mac.contains("launcher_outstanding(&m, platform, own, Some(&installed), false)")
                && mac.contains("let own = fixture.map_or_else(crate::self_update::own_build, |(_, own)| own);")
                && mac.contains("crate::cmd_setup::engine_update_row(&m, platform, gxio::game::discover().install_dir.is_some())")
                // and installs that engine from THIS document, never a cache re-read (Codex q7-macupd-r1)
                && mac.contains("crate::cmd_setup::install_engine_reporting(Some(&m),")
                && !mac.contains("gxcore::decide("),
            "the macOS press decides by a rule of its own:\n{mac}"
        );
    }

    /// The table holds `headline`; this holds that the screens PAINT it rather than choosing words
    /// of their own - which is what both did until the fifth review, out of every test's reach.
    #[test]
    fn both_screens_paint_the_launchers_headline() {
        let web = include_str!("../../../../design/launcher/screens/MainWeb.html");
        let at = web.find("function verdictText(s) {").expect("MainWeb's verdictText is gone");
        let body = &web[at..at + web[at..].find("\n  }\n").expect("verdictText closes")];
        assert!(
            body.contains("return s.headline || VERDICT_LABEL[s.verdict] || s.verdict;") && !body.contains("installed_from"),
            "MainWeb chooses its own headline:\n{body}"
        );
        let upd = include_str!("../../../../design/launcher/screens/MainUpdate.html");
        let at = upd.find("function paint(s, notes) {").expect("MainUpdate's paint is gone");
        let body = &upd[at..at + upd[at..].find("set(status, headline, colour);").expect("paint sets the status")];
        assert!(
            body.contains("var headline = s.headline || 'Update available';")
                && !body.contains("installed_from")
                && !body.contains("s.verdict === 'older' ?"),
            "MainUpdate chooses its own headline:\n{body}"
        );
    }
}
