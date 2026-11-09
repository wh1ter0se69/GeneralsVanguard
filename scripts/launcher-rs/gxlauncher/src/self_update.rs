// THE LAUNCHER'S OWN UPDATE, ON macOS - PRESSED, SINCE 28/09/2026.
//
// It used to run by itself, once per launch, from `main.rs`'s `.setup`, beside MainWeb's silent
// engine install ("check and install silently"). The owner has since asked for macOS to behave like
// Windows: a pending update turns the home screen yellow, hands over to MainUpdate, and is taken by
// pressing UPDATE. So nothing starts this but `cmd_release::release_apply`'s macOS arm
// ([`run_pressed`]), after the engine install when one is outstanding.
//
// WHAT RUNS. It asks one question - is a newer launcher published than THIS binary? - and if so it
// downloads that launcher, checks it against the signed document, waits until no engine install
// holds the bundles, exchanges the `.app` underneath the running process in one atomic step, starts
// the new one just far enough to hear it name itself, and only then lets the old one go. The press
// then relaunches (MainUpdate calls `relaunch` on `restart_needed`), so the new launcher takes over
// at once; if that restart fails it takes over at the next launch. The swap is safe underneath a
// running launcher: the exchange leaves this process on its own mapped, now unlinked, executable; the
// screens are compiled into that executable, not read from the bundle; and nothing else in the bundle
// is read after start (see `swap_checked`).
//
// WHAT IT NEVER DOES. Hold the bundle lock for its download - only for the extract, the exchange and
// the new launcher's one-line answer. Replace a development build. Swap while an engine install holds
// the bundles. Swap by anything but one atomic exchange: where the volume cannot exchange two names,
// the launcher the player has stays exactly where it is. Keep a launcher that does not start: one that
// does not answer is exchanged back out. Every failure is an ordinary outcome with a sentence, written
// to stderr and to `gxselfupdate.txt` in the state directory, and said on the Update page.
//
// macOS ONLY. Linux's pressed update is `cmd_release::release_apply`'s own Linux arm (an AppImage);
// Windows is GeneralsXUpdate.exe's.

use gxio::release as rel;

// NO PACKAGED LAUNCHER CARRIES THE FIXTURE, held by the compiler as well as by build.rs. build.rs
// refuses GX_SELFUPDATE_FIXTURE_BUILD=1 together with GX_COMMIT_EPOCH; this refuses the fixture's
// cfg together with a commit time whatever switched the cfg on - RUSTFLAGS="--cfg
// gx_selfupdate_fixture" reaches rustc without build.rs ever seeing it.
#[cfg(gx_selfupdate_fixture)]
const _: () = assert!(
    option_env!("GX_COMMIT_EPOCH").is_none(),
    "this build carries a commit time, so it is a packaged launcher, and a packaged launcher must \
     never contain the self-update fixture"
);

/// What THIS binary is. Compiled in by build.rs, never read off disk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct OwnBuild<'a> {
    /// The nine-character commit the packager names the artifact after
    /// (`GeneralsXLauncher-macos-<commit9>.tar.gz`) and `--gx-selftest` reports.
    pub commit9: &'a str,
    /// The commit's own time, `git show -s --format=%ct`, from GX_COMMIT_EPOCH. `None` for every
    /// build the packager did not make - `cargo run`, `cargo test`, a hand-run `cargo tauri build` -
    /// and such a build never replaces itself.
    pub commit_epoch: Option<u64>,
}

/// What the background job should do about a verified release document.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SelfUpdatePlan {
    /// A newer launcher is published, and this is its row.
    Install(gxcore::Artifact),
    /// This binary IS the published launcher. Nothing downloads; the install record is brought up
    /// to date, so the home screen's verdict says so on its next read.
    Current(gxcore::Artifact),
    /// Nothing to do, and the sentence that says why.
    Nothing(String),
}

/// THE RULE for "a newer launcher is published", whole:
///
///   1. This binary carries a commit time. A build with none is a development build and never
///      replaces itself.
///   2. The signed document has a launcher row for this platform, found with `applicable_row` -
///      the predicate the update panel counts with, so an engine payload, an `any` overlay or
///      another platform's launcher is never taken for this one.
///   3. That row's commit (the last dash token of its name) is not this binary's own. If it is,
///      this binary IS the published launcher: `Current`.
///   4. The document's ordinal is STRICTLY GREATER than this binary's commit time. Both are
///      `git show -s --format=%ct` of a commit - the ordinal of the release commit (gx-manifest.py),
///      the epoch of this binary's own - so this compares commit times, never the time anything
///      was packaged: a build packaged today from an older commit would otherwise refuse genuinely
///      newer releases. Equal is not newer.
///
/// Rule 4 is the DIRECTION GUARD, and it is not the rollback floor `choose_manifest` already
/// applies. That floor refuses a document older than one this machine has seen; it says nothing
/// about whether the launcher a document names is newer than the binary reading it. Without rule 4,
/// a launcher packaged before it is published would replace itself with the older published one on
/// its first launch.
///
/// ITS ONE BLIND SPOT, stated rather than hidden. The ordinal is the time of the commit the
/// document was generated at; the launcher row is named after the commit it was packaged at. Every
/// document published up to 2026-09-11 had both at one commit (measured that day: the live
/// ordinal, 1787709779, is the commit time of fc33f7efd, the live launcher row's commit), and then
/// rule 4 compares the two launchers' commit times exactly. A document generated later that still
/// carries an older launcher row would make rule 4 compare against a later time than that row's, so
/// a binary built between the two - an unpublished candidate, or a website build that is not the
/// manifest's row - would be replaced by the published launcher.
///
/// WHAT KEEPS IT SHUT IS A PUBLISHING RULE, not this code, because closing it here would need the
/// row's own commit time in the signed document, which every parser reads: the manifest's macOS
/// launcher row is the launcher the website serves, and the two are published together; and a
/// release commit is never rebased or amended after it is packaged, because `%ct` is the
/// COMMITTER's time and moves with either.
pub(crate) fn plan_self_update(
    m: &gxcore::Manifest,
    platform: &str,
    own: OwnBuild<'_>,
) -> SelfUpdatePlan {
    let Some(built) = own.commit_epoch else {
        return SelfUpdatePlan::Nothing(DEVELOPMENT_BUILD.to_string());
    };
    let Some(row) = m
        .artifacts
        .iter()
        .find(|a| crate::cmd_release::applicable_row(a, platform))
    else {
        return SelfUpdatePlan::Nothing(format!(
            "The published release names no {platform} launcher, so there is nothing to install."
        ));
    };
    if crate::cmd_release::commit_in_artifact_name(&row.name).as_deref() == Some(own.commit9) {
        return SelfUpdatePlan::Current(row.clone());
    }
    if m.ordinal <= built {
        return SelfUpdatePlan::Nothing(format!(
            "The published launcher (#{}) is not newer than this one (#{built}), so it is not \
             installed.",
            m.ordinal
        ));
    }
    SelfUpdatePlan::Install(row.clone())
}

/// Said, and logged, whenever a development build is asked to update itself.
const DEVELOPMENT_BUILD: &str =
    "This launcher is a development build (it carries no commit time), so it never replaces itself.";

/// How one run of the background job ended. Every arm is an ordinary outcome with a sentence.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SelfUpdateOutcome {
    /// Nothing newer, nothing this build may do, or an artifact that failed within the last day.
    /// Not shown on the home screen.
    Nothing(String),
    /// This binary is the published launcher; `recorded` says whether the record now says so.
    Current { recorded: bool },
    /// A newer launcher is published and this copy cannot replace itself - a translocated bundle,
    /// a folder this account cannot write, a volume that cannot exchange two names in one step.
    /// Nothing moved. The sentence says what the player can do about it.
    Unavailable(String),
    /// An engine install held the bundles for the whole wait, or another launcher process was
    /// replacing the launcher. Nothing moved. Tried again at the next launch, and never counted as
    /// the artifact failing.
    Deferred(String),
    /// The download, its check, the unpacking or the new launcher's answer failed. The launcher the
    /// player had is in place. That artifact is tried again after a day.
    Failed(String),
    /// Swapped, and the new launcher answered. It takes over the next time the launcher opens.
    Installed { name: String, recorded: bool },
}

/// Where the background update got to. MainWeb reads `u.phase`, `u.detail` and `u.name`.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct SelfUpdateStatusDto {
    /// idle | downloading | waiting | nothing | current | unavailable | deferred | failed | installed
    pub phase: String,
    /// The sentence behind the phase, or empty.
    pub detail: String,
    /// The artifact the phase is about, or empty.
    pub name: String,
}

impl SelfUpdateOutcome {
    /// What the home screen is told about this outcome.
    pub(crate) fn status(&self) -> SelfUpdateStatusDto {
        let (phase, detail, name) = match self {
            Self::Nothing(why) => ("nothing", why.clone(), String::new()),
            Self::Current { .. } => ("current", String::new(), String::new()),
            Self::Unavailable(why) => ("unavailable", why.clone(), String::new()),
            Self::Deferred(why) => ("deferred", why.clone(), String::new()),
            Self::Failed(why) => ("failed", why.clone(), String::new()),
            Self::Installed { name, .. } => (
                "installed",
                "It takes over the next time you open the launcher.".to_string(),
                name.clone(),
            ),
        };
        SelfUpdateStatusDto { phase: phase.to_string(), detail, name }
    }
}

/// Why a new launcher was not kept: from [`swap_checked`], from the staging before it, and from
/// the swap lock taken around both.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SwapRefused {
    /// Nothing was exchanged, so nothing moved: the volume cannot exchange two names in one step,
    /// or the running form is not a macOS bundle. The launcher that is running is untouched.
    Unavailable(String),
    /// The archive did not unpack into an application, or the new launcher was exchanged in, did
    /// not answer, and was exchanged back out. The launcher the player had is in place.
    Failed(String),
    /// The new launcher did not answer and could NOT be exchanged back. It stays in place, and the
    /// previous launcher is left in the stage beside it - the caller must keep that stage, and a
    /// later start's sweep removes it once it is a day old.
    Stuck(String),
    /// Another launcher process holds the swap lock: it is replacing the launcher right now.
    /// Nothing was unpacked and nothing moved; this process leaves the swap to that one.
    Busy(String),
}

/// What starting a swapped-in launcher with the identify flag produced.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ProbeRun {
    /// It ended by itself: its exit code (`None` when a signal ended it) and what it printed.
    Exited { code: Option<i32>, stdout: String },
    /// It had not ended when [`PROBE_TIMEOUT`] ran out, and was killed.
    TimedOut,
    /// It could not be started at all; an executable for another architecture is refused here.
    NotStarted(String),
}

/// How long a swapped-in launcher has to answer. It answers before `tauri::Builder` runs, so a
/// working one takes a fraction of a second; this is for a disk that is slow to page in a cold
/// binary. PLAY's closer waits at most this long for it, under the bundle lock.
pub(crate) const PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// How long an artifact that failed to install, or could not be installed here, is left alone
/// before it is tried again. A newer artifact is tried at once.
pub(crate) const RETRY_AFTER_SECS: u64 = 24 * 60 * 60;

/// THE NEW LAUNCHER'S ANSWER, judged. It must have ended by itself with exit code 0, and its first
/// line must name `commit9` - the commit its release row is named after - and a commit time of its
/// own.
///
/// WHAT THIS CATCHES: an executable for another architecture (refused at exec), a signature the
/// kernel kills on exec, a missing framework (dyld aborts), a crash early in start, a build of
/// another commit, and a development build, which would never update itself again.
///
/// WHAT IT CANNOT: that the new launcher's webview paints. The answer is printed before
/// `tauri::Builder` runs, so no window ever opens - which is why --gx-selftest, which shows the
/// window and requires it to be visible, cannot be the probe. The packager's SMOKE check proves the
/// paint on the machine that built the release.
pub(crate) fn probe_verdict(run: &ProbeRun, commit9: &str) -> Result<(), String> {
    let stdout = match run {
        ProbeRun::Exited { code: Some(0), stdout } => stdout,
        ProbeRun::Exited { code: Some(code), .. } => {
            return Err(format!(
                "The new launcher stopped with exit code {code} instead of answering."
            ))
        }
        ProbeRun::Exited { code: None, .. } => {
            return Err(
                "The new launcher was stopped by the system instead of answering.".to_string()
            )
        }
        ProbeRun::TimedOut => {
            return Err(format!(
                "The new launcher did not answer within {} seconds.",
                PROBE_TIMEOUT.as_secs()
            ))
        }
        ProbeRun::NotStarted(why) => {
            return Err(format!("The new launcher could not be started: {why}."))
        }
    };
    let said: serde_json::Value = stdout
        .lines()
        .next()
        .and_then(|l| serde_json::from_str(l).ok())
        .unwrap_or(serde_json::Value::Null);
    let commit = said.get("commit").and_then(|c| c.as_str()).unwrap_or("");
    if commit != commit9 {
        return Err(format!(
            "The new launcher says it is `{commit}`, not the `{commit9}` the release names."
        ));
    }
    if said.get("commit_epoch").and_then(|e| e.as_u64()).unwrap_or(0) == 0 {
        return Err(
            "The new launcher carries no commit time, so it would never update itself again."
                .to_string(),
        );
    }
    Ok(())
}

/// THE UNATTENDED SWAP: one atomic exchange, proof that the new launcher starts, and only then is
/// the previous one let go.
///
///   1. `exchange` the staged `fresh` bundle with the installed `app`: one
///      `renamex_np(RENAME_SWAP)` in production (`gxio::update::exchange_bundles`), never the two
///      renames a pressed update falls back to. Refused: nothing moved, the running launcher stays
///      where the player keeps it, and the answer is `Unavailable`.
///   2. `probe` the bundle now at `app` - production starts its executable with the identify flag
///      under [`PROBE_TIMEOUT`] - and judge the answer ([`probe_verdict`]).
///   3. It answered: the previous launcher, now at `fresh`, is removed. Not before.
///   4. It did not: exchange again, which puts the previous launcher back, atomically again, and
///      the refused one is removed (`Failed`). If that exchange is refused too, the new launcher
///      stays, and the previous one is left at `fresh` and named in the sentence (`Stuck`). Nothing
///      here removes it; a later start's sweep does, once its stage is a day old.
///
/// Between 1 and 3 the installed name holds a launcher that has not answered yet, for at most
/// [`PROBE_TIMEOUT`]. PLAY's closer waits it out under the bundle lock. The red close button and
/// Cmd+Q do not: a quit inside it leaves the new launcher in place and the previous one in the
/// hidden stage, which a later start's sweep removes once it is a day old.
pub(crate) fn swap_checked(
    fresh: &std::path::Path,
    app: &std::path::Path,
    exchange: &dyn Fn(&std::path::Path, &std::path::Path) -> Result<(), String>,
    probe: &dyn Fn(&std::path::Path) -> ProbeRun,
    commit9: &str,
) -> Result<(), SwapRefused> {
    if let Err(why) = exchange(fresh, app) {
        return Err(SwapRefused::Unavailable(format!(
            "{why} The launcher replaces itself only by one atomic exchange, so the version you \
             have stays in place."
        )));
    }
    let answer = probe_verdict(&probe(app), commit9);
    let Err(why) = answer else {
        // Only now is the previous launcher let go.
        let _ = std::fs::remove_dir_all(fresh);
        return Ok(());
    };
    match exchange(fresh, app) {
        Ok(()) => {
            let _ = std::fs::remove_dir_all(fresh);
            Err(SwapRefused::Failed(format!(
                "{why} It was exchanged back out, and the launcher you had is in place."
            )))
        }
        Err(e) => {
            // The previous launcher is at `fresh` now, and it is the only copy. Nothing here removes
            // it - a later start's sweep does, once its stage is a day old - so the sentence says
            // where it is.
            Err(SwapRefused::Stuck(format!(
                "{why} Putting the previous launcher back failed too ({e}); it is at {}, and the \
                 new one stays in place.",
                fresh.display()
            )))
        }
    }
}

/// Every effect `run_self_update` has on the world, supplied so a test can watch or refuse each
/// one. The same `&dyn Fn` shape as `gxio::update::swap_bundle`'s rename and swap.
pub(crate) struct SelfUpdateSeams<'a, G> {
    /// Where this launcher is, or why it cannot be replaced (`gxio::update::locate_target`).
    pub locate: &'a dyn Fn() -> Result<gxio::update::Target, String>,
    /// Fetch `url` to `dest`, exactly `size` bytes (`gxio::update::download_to_file`).
    pub download: &'a dyn Fn(&str, &std::path::Path, u64) -> Result<u64, String>,
    /// Take the bundles, or `None` if an engine install still holds them after the wait.
    pub hold_bundles: &'a dyn Fn() -> Option<G>,
    /// Put the verified archive in place and hold the new launcher to the commit given: staged by
    /// `gxio::update::stage_macos_tgz`, then [`swap_checked`].
    pub apply: &'a dyn Fn(&std::path::Path, &gxio::update::Target, &str) -> Result<(), SwapRefused>,
    /// Write the install record; false when it could not be written.
    pub record: &'a dyn Fn(&gxio::update::Target, &gxcore::InstallState) -> bool,
    /// Tell the home screen where the job has got to: (phase, artifact name).
    pub report: &'a dyn Fn(&str, &str),
    /// When an artifact last failed to install, and which one.
    pub last_failed: &'a dyn Fn() -> Option<(u64, String)>,
    /// Remember that the named artifact failed to install at the given time.
    pub failed: &'a dyn Fn(&str, u64),
    /// Unix seconds, now.
    pub now: &'a dyn Fn() -> u64,
}

/// Where THIS process stages the download: `<staging>/<pid>.<artifact>`.
///
/// THE PID IS THE POINT. `release_apply` stages at `<staging>/<artifact>`, a name every launcher
/// process shares, and `download_to_file` deletes whatever is at its destination before it starts -
/// so two launchers updating at once would delete each other's download mid-transfer. A name that
/// carries the pid cannot be another live process's. The artifact name stays as the suffix so the
/// folder still says what it holds, and `prune_staging` ages it out like anything else there.
pub(crate) fn staged_download(
    staging: &std::path::Path,
    pid: u32,
    artifact: &str,
) -> std::path::PathBuf {
    staging.join(format!("{pid}.{artifact}"))
}

/// ORDER IS THE WHOLE DESIGN: plan -> the day's backoff -> locate -> download -> VERIFY -> hold the
/// bundles -> exchange -> the new launcher answers -> release the bundles -> record. Nothing is
/// fetched for a copy that cannot be replaced, nothing reaches the swap unhashed, nothing swaps
/// while an engine install holds the bundles, and nothing is kept that has not answered. `pid`
/// names the staged file (see `staged_download`); production passes `std::process::id()`.
///
/// THE SWAP IS SAFE UNDER THE RUNNING LAUNCHER, which is why nothing relaunches afterwards. The
/// archive is unpacked beside the bundle, and the two directories are exchanged by one
/// `renamex_np(RENAME_SWAP)` or not at all - never by the two renames a pressed update falls back
/// to, between which the installed name is empty. The running executable is mapped, and an
/// unlinked file stays readable to the process that has it mapped; its code signature was loaded by
/// the kernel at exec, so page-ins keep validating against it. The screens are compiled into that
/// executable (tauri-codegen's embedded assets), so every page this process loads afterwards comes
/// from memory, not from the bundle. The one thing the bundle still offers is
/// `Contents/Resources/icon.icns` and `Info.plist`, which the OS reads by path - and the new bundle
/// has both, at the same paths.
///
/// THE BUNDLE LOCK IS HELD FOR THE SWAP ONLY: taken after the download and its check, dropped
/// before the record is written, so PLAY's closer and an engine install wait for an extract, an
/// exchange and the new launcher's answer, and never for a download.
///
/// NO RETRY STORM. An artifact that failed, or could not be installed here, is left alone for
/// [`RETRY_AFTER_SECS`]: re-downloading, on every launch, a launcher that macOS refuses to install
/// or that does not match its signature is the storm. A newer artifact is tried at once. A failure
/// stamped after `now` is a clock that moved, and holds nothing back.
pub(crate) fn run_self_update<G>(
    m: &gxcore::Manifest,
    installed: gxcore::InstallState,
    platform: &str,
    own: OwnBuild<'_>,
    staging: &std::path::Path,
    pid: u32,
    seams: &SelfUpdateSeams<'_, G>,
) -> SelfUpdateOutcome {
    let art = match plan_self_update(m, platform, own) {
        SelfUpdatePlan::Nothing(why) => return SelfUpdateOutcome::Nothing(why),
        SelfUpdatePlan::Current(row) => {
            // Brought up to date where a later update will not destroy it, which is why the
            // target is located first: `install_record_target` skips anything inside it.
            let recorded = match (seams.locate)() {
                Ok(target) => (seams.record)(
                    &target,
                    &crate::cmd_release::record_one_applied(installed, &row, m.ordinal),
                ),
                Err(_) => false,
            };
            return SelfUpdateOutcome::Current { recorded };
        }
        SelfUpdatePlan::Install(row) => row,
    };

    let now = (seams.now)();
    if let Some((at, name)) = (seams.last_failed)() {
        let within_a_day = now.checked_sub(at).is_some_and(|age| age < RETRY_AFTER_SECS);
        if name == art.name && within_a_day {
            return SelfUpdateOutcome::Nothing(format!(
                "{} did not install at {at}; the launcher tries it again after {}, and a newer \
                 launcher as soon as one is published.",
                art.name,
                at + RETRY_AFTER_SECS
            ));
        }
    }
    let outcome = try_install(m, installed, platform, staging, pid, &art, seams);
    if matches!(outcome, SelfUpdateOutcome::Failed(_) | SelfUpdateOutcome::Unavailable(_)) {
        (seams.failed)(&art.name, now);
    }
    outcome
}

/// Everything after the rule and the backoff: locate, fetch, verify, hold, swap, record.
fn try_install<G>(
    m: &gxcore::Manifest,
    installed: gxcore::InstallState,
    platform: &str,
    staging: &std::path::Path,
    pid: u32,
    art: &gxcore::Artifact,
    seams: &SelfUpdateSeams<'_, G>,
) -> SelfUpdateOutcome {
    // Located only now that a newer launcher is known to exist: a copy that cannot replace itself
    // is worth a sentence to the player only when there is something to replace it with.
    let target = match (seams.locate)() {
        Ok(t) => t,
        Err(why) => return SelfUpdateOutcome::Unavailable(why),
    };
    // The selector the irreversible swap uses everywhere: the row must also be one THIS running
    // form can be rebuilt from.
    if crate::cmd_release::select_launcher_artifact(std::slice::from_ref(art), platform, &target)
        .is_none()
    {
        return SelfUpdateOutcome::Nothing(format!(
            "{} is not something this copy of the launcher can be replaced from.",
            art.name
        ));
    }

    let dest = staged_download(staging, pid, &art.name);
    // `base` carries no scheme and ends in '/', e.g. "dl.wh1ter0se.dev/"; download_to_file refuses
    // anything that is not https://.
    let url = format!("https://{}{}", m.base, art.name);
    (seams.report)("downloading", &art.name);
    if let Err(e) = (seams.download)(&url, &dest, art.size) {
        let _ = std::fs::remove_file(&dest);
        return SelfUpdateOutcome::Failed(e);
    }
    // verify_staged removes the file itself when it refuses it.
    if let Err(e) = gxio::update::verify_staged(&dest, &art.name, &art.hash) {
        return SelfUpdateOutcome::Failed(e);
    }

    (seams.report)("waiting", &art.name);
    let Some(bundles) = (seams.hold_bundles)() else {
        let _ = std::fs::remove_file(&dest);
        return SelfUpdateOutcome::Deferred(
            "An engine install was still running, so the launcher's own update waits for the next \
             launch."
                .to_string(),
        );
    };
    // The commit the new launcher must name when it answers: the one its release row is named
    // after. A row with no readable commit gives "", which no launcher names, so it is refused.
    let expected = crate::cmd_release::commit_in_artifact_name(&art.name).unwrap_or_default();
    let applied = (seams.apply)(&dest, &target, &expected);
    drop(bundles);
    let _ = std::fs::remove_file(&dest);
    match applied {
        Ok(()) => {}
        Err(SwapRefused::Unavailable(why)) => return SelfUpdateOutcome::Unavailable(why),
        Err(SwapRefused::Busy(why)) => return SelfUpdateOutcome::Deferred(why),
        Err(SwapRefused::Failed(why) | SwapRefused::Stuck(why)) => {
            return SelfUpdateOutcome::Failed(why)
        }
    }

    let recorded = (seams.record)(
        &target,
        &crate::cmd_release::record_one_applied(installed, art, m.ordinal),
    );
    SelfUpdateOutcome::Installed { name: art.name.clone(), recorded }
}

/// THE LOCK THE LAUNCHER'S OWN SWAP AND THE ENGINE INSTALL SHARE, so the one never lands inside the
/// other: the owner's rule is that the launcher never swaps itself in the middle of an engine
/// install.
///
/// Who holds it, and for how long:
///   * `cmd_setup::setup_install_engine`, for the whole install, taken right after its claim;
///   * `run_self_update`, for the extract, the exchange and the new launcher's answer - never for
///     its download;
///   * PLAY's closer and the relaunch (`cmd_game::close_after_handover`), after the install claim
///     and across the end of the process, so a swap already under way finishes first.
/// The order is the same everywhere two of them nest: install claim, then this, then the swap lock
/// ([`try_swap_lock`], which only the swap takes, and never waits for), then gxio's COMMIT, which
/// nothing takes while it holds the swap lock.
///
/// NOT EVERY WRITE TO A BUNDLE. Moving an installed engine to the other location
/// (`setup_apply_choices`) takes only gxio's COMMIT lock; it touches the engine's bundle, never the
/// launcher's, and neither needs nor takes this.
///
/// IN-PROCESS, the same honest scope `ENGINE_INSTALL_RUNNING` documents: two launcher windows are
/// one process and are what this closes. A second launcher PROCESS is kept out of the swap by the
/// swap lock ([`try_swap_lock`]), a file lock in the state directory that the swap takes inside
/// this one and holds from the extract to the last exchange. Without it the two processes'
/// exchanges and exchanges back could interleave: a launcher that never answered would be left
/// where the player keeps it, and the previous one deleted. A second process's download needs
/// neither lock, because it is staged under its own pid; a second process's engine install is not
/// seen here at all, and it replaces the engine's bundle, never the launcher's.
///
/// NEVER TAKEN BY A TEST. The tests run in parallel in one binary, and a test holding this would
/// stall every other test that reaches production code taking it. Everything that takes it takes it
/// through a parameter - `lock_bundles`, `wait_for_bundles`, the closer's `hold_bundles` - and the
/// tests pass a lock of their own. `no_test_takes_the_real_bundle_lock` holds that, for every
/// spelling that reaches it: a call, a path or a function pointer.
///
/// ON LINUX IT IS TAKEN AND INERT. The engine install and the closers take it on every platform,
/// and nothing else ever holds it there, because the background update runs on macOS only.
pub(crate) static BUNDLE_WORK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Take `lock`, waiting as long as it takes. A POISONED lock is taken: whatever panicked while it
/// held the bundles has already let go of them, and refusing forever would turn one panic into a
/// launcher that never updates itself and never closes.
pub(crate) fn lock_bundles(lock: &std::sync::Mutex<()>) -> std::sync::MutexGuard<'_, ()> {
    lock.lock().unwrap_or_else(|e| e.into_inner())
}

/// The real lock, for the places that hold the bundles until they are done: the engine install and
/// the two closers. They take it unbounded because what they wait for is bounded - a swap is an
/// extract, an exchange and an answer capped at `PROBE_TIMEOUT`, and the closers only wait after
/// the install claim is theirs.
pub(crate) fn claim_bundle_work() -> std::sync::MutexGuard<'static, ()> {
    lock_bundles(&BUNDLE_WORK)
}

/// Try `lock`, calling `pause` between tries, for up to `budget`. `None` means somebody - an engine
/// install - still holds it, and the launcher's swap then waits for the next launch rather than land
/// inside it. A poisoned lock is taken, for the reason `lock_bundles` gives.
///
/// `pause` is where production sleeps between tries. It is a parameter so a test can let the
/// "install" finish in the middle of the wait and see it waited for - a wait that gave up at once
/// would otherwise pass every test that holds the lock throughout.
pub(crate) fn wait_for_bundles<'l>(
    lock: &'l std::sync::Mutex<()>,
    budget: std::time::Duration,
    pause: &dyn Fn(),
) -> Option<std::sync::MutexGuard<'l, ()>> {
    let deadline = std::time::Instant::now() + budget;
    loop {
        match lock.try_lock() {
            Ok(held) => return Some(held),
            Err(std::sync::TryLockError::Poisoned(p)) => return Some(p.into_inner()),
            Err(std::sync::TryLockError::WouldBlock) => {}
        }
        if std::time::Instant::now() >= deadline {
            return None;
        }
        pause();
    }
}


/// THE SWAP LOCK: the launcher's own swap, across launcher PROCESSES. [`BUNDLE_WORK`] is a mutex,
/// and a mutex ends where its process does; two launchers opened one after the other are two
/// processes, and each runs its own background update. So the swap also takes this, a file lock
/// on [`SWAP_LOCK_FILE`] in the state directory, before it unpacks the new launcher, and holds it
/// until the stage is settled: one process's exchange, the new launcher's answer and any exchange
/// back can never be interleaved with another's. It is taken inside `BUNDLE_WORK` and never waited
/// for - a process that finds it held leaves the swap to the one that holds it.
///
/// `flock`, through `std::fs::File::try_lock`, and held by the open file rather than the process:
/// a second holder in the same process is refused too. The kernel lets go of it when the last copy
/// of that open file is closed - when it is dropped, or a moment later if this process was starting
/// another program at that instant, which carries a copy until it runs - or when the process ends,
/// however it ends; so a launcher that crashes or is force-quit while it holds it never leaves it
/// held. The file is never written and never removed: removing it while one process held it would
/// let the next lock a new file of the same name, and both would hold "the" lock.
pub(crate) struct SwapLock {
    _file: std::fs::File,
}

/// The swap lock's file, in the state directory beside `gxselfupdate.txt`: one file for every
/// launcher process of this account.
pub(crate) const SWAP_LOCK_FILE: &str = "gxselfupdate.lock";

/// Take the swap lock at `path` without waiting. `Ok(None)` is another holder - another launcher
/// process - holding it; an error is a lock that cannot be opened or taken at all. Its folder is
/// created if it is missing.
pub(crate) fn try_swap_lock(path: &std::path::Path) -> Result<Option<SwapLock>, String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)
            .map_err(|e| format!("Could not prepare {} for the swap lock: {e}.", dir.display()))?;
    }
    let file = std::fs::File::options()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(path)
        .map_err(|e| format!("Could not open the swap lock {}: {e}.", path.display()))?;
    match file.try_lock() {
        Ok(()) => Ok(Some(SwapLock { _file: file })),
        Err(std::fs::TryLockError::WouldBlock) => Ok(None),
        Err(std::fs::TryLockError::Error(e)) => {
            Err(format!("Could not take the swap lock {}: {e}.", path.display()))
        }
    }
}
/// One line for stderr and for gxselfupdate.txt:
/// `<unix seconds> <phase> launcher=<this binary's commit> <artifact> <sentence>`.
pub(crate) fn log_line(s: &SelfUpdateStatusDto, commit9: &str, at: u64) -> String {
    format!("{at} {} launcher={commit9} {} {}\n", s.phase, s.name, s.detail)
}

/// A commit time is a positive number of seconds, and anything else is none at all - a development
/// build. build_env.rs already refuses anything else at build time, so this is the second line; it
/// is also the only place a broken reading would show, since only a packaged build carries a value.
pub(crate) fn parse_epoch(raw: Option<&str>) -> Option<u64> {
    raw.map(str::trim).and_then(|s| s.parse::<u64>().ok()).filter(|&n| n > 0)
}

/// The backoff file's one line: `<unix seconds> <artifact>`, the last artifact that failed to
/// install and when.
pub(crate) fn backoff_line(at: u64, artifact: &str) -> String {
    format!("{at} {artifact}\n")
}

/// That line back. Anything that is not exactly one holds nothing back: a file this launcher did
/// not write must never keep an update away.
pub(crate) fn parse_backoff(text: &str) -> Option<(u64, String)> {
    let mut fields = text.split_whitespace();
    let at = fields.next()?.parse().ok()?;
    let artifact = fields.next()?.to_string();
    if fields.next().is_some() {
        return None;
    }
    Some((at, artifact))
}

/// The event the background update reports on. MainWeb listens for it by this name.
pub const SELF_UPDATE_EVENT: &str = "gx-self-update";

/// The flag a swapped-in launcher is started with, to prove it starts: `main` answers with
/// [`identify_line`] and exits before anything else runs.
pub(crate) const IDENTIFY_FLAG: &str = "--gx-identify";

/// What a launcher says when it is asked who it is: one line of JSON with its commit and its
/// commit time, 0 for a development build.
pub(crate) fn identify_line(own: OwnBuild<'_>) -> String {
    serde_json::json!({
        "gxidentify": 1,
        "commit": own.commit9,
        "commit_epoch": own.commit_epoch.unwrap_or(0),
    })
    .to_string()
}

/// This binary's own identity. The commit time is read through [`parse_epoch`], the one reading of
/// it a test holds.
pub(crate) fn own_build() -> OwnBuild<'static> {
    OwnBuild {
        commit9: env!("GX_COMMIT9"),
        commit_epoch: parse_epoch(option_env!("GX_COMMIT_EPOCH")),
    }
}

/// Did whoever started this process ask it who it is? See [`identify_and_exit`].
pub(crate) fn identify_requested() -> bool {
    std::env::args_os().skip(1).any(|a| a == IDENTIFY_FLAG)
}

/// Say who this build is, and end the process. `main` calls this first, before anything is written
/// and before any window exists, so the background update can start a swapped-in launcher to hear
/// it name itself and nothing else happens. Flushed explicitly, for the reason
/// `gx_selftest_report` gives: an exit runs no buffered writer's destructor.
pub(crate) fn identify_and_exit() -> ! {
    use std::io::Write;
    println!("{}", identify_line(own_build()));
    let _ = std::io::stdout().flush();
    std::process::exit(0);
}

/// Start `exe` with the identify flag and wait for it to end, for at most `timeout`.
///
/// KILLED, AND REAPED, WHEN THE TIME RUNS OUT, so a new launcher that hangs costs this process a
/// wait and nothing else - never a stray process left running. Its stdout is read once it has
/// ended; an answer is one short line, so the pipe never fills while it runs.
pub(crate) fn run_identify(exe: &std::path::Path, timeout: std::time::Duration) -> ProbeRun {
    use std::io::Read;
    let mut child = match std::process::Command::new(exe)
        .arg(IDENTIFY_FLAG)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
    {
        Ok(child) => child,
        Err(e) => return ProbeRun::NotStarted(e.to_string()),
    };
    let deadline = std::time::Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                let mut stdout = String::new();
                if let Some(mut out) = child.stdout.take() {
                    let _ = out.read_to_string(&mut stdout);
                }
                return ProbeRun::Exited { code: status.code(), stdout };
            }
            Ok(None) if std::time::Instant::now() < deadline => {
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return ProbeRun::TimedOut;
            }
        }
    }
}

/// The executable inside `bundle`, named as this running one is: the packager gives every launcher
/// build the same name (tauri.conf.json's `mainBinaryName`).
fn bundle_executable(bundle: &std::path::Path) -> std::path::PathBuf {
    let name = std::env::current_exe()
        .ok()
        .and_then(|p| p.file_name().map(|n| n.to_os_string()))
        .unwrap_or_else(|| "GeneralsX Launcher".into());
    bundle.join("Contents").join("MacOS").join(name)
}

/// THE UNATTENDED APPLY, whole, as production runs it (`with_production_seams`): take the swap
/// lock, unpack the verified archive beside `bundle`, [`swap_checked`], and settle the stage.
///
/// THE SWAP LOCK FIRST, before anything is unpacked, and held until the stage is settled, so no
/// other launcher process can exchange, answer or exchange back in between (see [`SwapLock`]).
/// `swap_lock` is `take_swap_lock` in production. `Ok(None)` - another launcher process holds it -
/// is `Busy`, and nothing is unpacked or moved; a lock that cannot be taken at all is `Unavailable`.
///
/// THE STAGE IS SETTLED BY WHAT THE SWAP LEFT IN IT: whichever launcher is not installed - the
/// refused new one, or the previous one once the new one has answered - and either way it goes.
/// After `Stuck` it holds the previous launcher, the only copy there is, and it stays. Nothing here
/// removes it, the sentence names where it is, and a later start's sweep removes it once it is a
/// day old.
pub(crate) fn apply_launcher<L>(
    archive: &std::path::Path,
    bundle: &std::path::Path,
    commit9: &str,
    swap_lock: &dyn Fn() -> Result<Option<L>, String>,
    exchange: &dyn Fn(&std::path::Path, &std::path::Path) -> Result<(), String>,
    probe: &dyn Fn(&std::path::Path) -> ProbeRun,
) -> Result<(), SwapRefused> {
    let held = swap_lock().map_err(|why| {
        SwapRefused::Unavailable(format!(
            "{why} The launcher replaces itself only while it holds its swap lock, so the version \
             you have stays in place."
        ))
    })?;
    let Some(_swapping) = held else {
        return Err(SwapRefused::Busy(
            "Another GeneralsX launcher is replacing the launcher right now, so this one leaves it \
             to that one."
                .to_string(),
        ));
    };
    let staged = gxio::update::stage_macos_tgz(archive, bundle).map_err(SwapRefused::Failed)?;
    let swapped = swap_checked(staged.fresh(), bundle, exchange, probe, commit9);
    if !matches!(swapped, Err(SwapRefused::Stuck(_))) {
        staged.discard();
    }
    swapped
}

/// The fixture folder's one file.
#[cfg_attr(not(gx_selfupdate_fixture), allow(dead_code))]
const FIXTURE_FILE: &str = "fixture.txt";

/// What a fixture says: who this binary pretends to be, and the one-row document it is handed.
#[cfg_attr(not(any(test, gx_selfupdate_fixture)), allow(dead_code))]
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Fixture {
    pub own_commit9: String,
    pub own_epoch: u64,
    pub row_name: String,
    pub manifest: gxcore::Manifest,
}

/// `fixture.txt`, three lines, read whole or refused:
///
/// ```text
/// own <commit9> <commit time>
/// document <ordinal>
/// row <artifact name> <22-character hash> <size>
/// ```
///
/// The row is a macOS row, as the published launcher row is. A line that is none of these, or one
/// that is missing, refuses the whole file: a fixture read in part would exercise something other
/// than what its author wrote down.
#[cfg_attr(not(any(test, gx_selfupdate_fixture)), allow(dead_code))]
pub(crate) fn parse_fixture(text: &str) -> Result<Fixture, String> {
    let mut own: Option<(String, u64)> = None;
    let mut ordinal: Option<u64> = None;
    let mut row: Option<gxcore::Artifact> = None;
    for line in text.lines().map(str::trim).filter(|l| !l.is_empty()) {
        let f: Vec<&str> = line.split_whitespace().collect();
        let number = |s: &str| s.parse::<u64>().map_err(|_| format!("`{line}` has no usable number"));
        match f.as_slice() {
            ["own", commit, epoch] => own = Some((commit.to_string(), number(epoch)?)),
            ["document", ord] => ordinal = Some(number(ord)?),
            ["row", name, hash, size] => {
                row = Some(gxcore::Artifact {
                    platform: "macos".to_string(),
                    name: name.to_string(),
                    hash: hash.to_string(),
                    size: number(size)?,
                })
            }
            _ => return Err(format!("`{line}` is not an own, document or row line")),
        }
    }
    let (own_commit9, own_epoch) = own.ok_or("there is no `own <commit9> <commit time>` line")?;
    let ordinal = ordinal.ok_or("there is no `document <ordinal>` line")?;
    let row = row.ok_or("there is no `row <name> <hash> <size>` line")?;
    Ok(Fixture {
        own_commit9,
        own_epoch,
        row_name: row.name.clone(),
        manifest: gxcore::Manifest {
            version: gxcore::MANIFEST_VERSION,
            ordinal,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: String::new(),
            relays: Vec::new(),
            base: "fixture.invalid/".to_string(),
            artifacts: vec![row],
        },
    })
}

/// Thirty minutes: longer than an 86 MB engine download on a slow link plus its apply. Past it the
/// swap waits for the next launch.
const BUNDLE_WAIT: std::time::Duration = std::time::Duration::from_secs(30 * 60);

/// How often a waiting swap asks again: a rate limit, not a resolution target.
const BUNDLE_POLL: std::time::Duration = std::time::Duration::from_millis(500);

/// The last outcome, in the state directory, because the stderr of a launcher opened from Finder
/// goes nowhere and whoever asks why the launcher is still old needs somewhere to look.
const SELF_UPDATE_LOG: &str = "gxselfupdate.txt";

/// Beside it, the artifact that last failed to install and when: what keeps a failure from being
/// downloaded again on every launch.
const SELF_UPDATE_BACKOFF: &str = "gxselfupdate-backoff.txt";

/// The last status reported, for a screen that loads after the report was sent.
static SELF_UPDATE_STATUS: std::sync::Mutex<Option<SelfUpdateStatusDto>> =
    std::sync::Mutex::new(None);

/// Where the background update has got to. Reads what the job last reported and starts nothing.
#[tauri::command]
pub fn release_self_update_status() -> SelfUpdateStatusDto {
    SELF_UPDATE_STATUS
        .lock()
        .ok()
        .and_then(|s| s.clone())
        .unwrap_or_else(|| SelfUpdateStatusDto {
            phase: "idle".to_string(),
            detail: String::new(),
            name: String::new(),
        })
}

/// THE LAUNCHER'S OWN UPDATE, PRESSED - `cmd_release::release_apply`'s macOS arm, and since 28/09/2026
/// the only thing that runs it. It used to start by itself from `main.rs`'s `.setup` on every macOS
/// launch; the owner asked for macOS to behave like Windows, which updates when UPDATE is pressed and
/// never unasked, so nothing starts it any more but the press.
///
/// THE SAME RUN THE BACKGROUND JOB MADE, not a second updater: [`run_self_update`] with production's
/// seams - locate, fetch, verify, hold the bundles, one atomic exchange, the new launcher must answer,
/// record. Two differences, both earned by a press: the download reports its bytes (`progress`, onto
/// the Update page's bar), and the day's backoff is not consulted - the press IS the retry, and a
/// refusal to try would leave UPDATE doing nothing. A failure is still written to the backoff file,
/// which nothing reads now. The outcome is logged and published as before, so MainWeb still says
/// where the last one got to.
pub(crate) fn run_pressed(
    app: &tauri::AppHandle,
    m: &gxcore::Manifest,
    installed: gxcore::InstallState,
    progress: &dyn Fn(u64),
) -> SelfUpdateOutcome {
    let outcome = pressed_once(app, m, installed, progress);
    let s = outcome.status();
    let line = log_line(&s, own_build().commit9, now_secs());
    eprint!("[gx] self-update: {line}");
    // Best effort: a state directory that cannot be written costs this record and nothing else.
    let _ = rel::write_cache_atomic(SELF_UPDATE_LOG, &line);
    publish(app, s);
    outcome
}

fn pressed_once(
    app: &tauri::AppHandle,
    m: &gxcore::Manifest,
    installed: gxcore::InstallState,
    progress: &dyn Fn(u64),
) -> SelfUpdateOutcome {
    #[cfg(gx_selfupdate_fixture)]
    {
        // THE SAME SNAPSHOT the verdict and the press judged (fixture_world): `m` is its document,
        // handed in by release_apply, and its identity is read from the same OnceLock - never a
        // second read of fixture.txt that could name another artifact (Codex q7-macupd-r7).
        if let (Some(dir), Some((_, own))) = (fixture_dir(), fixture_world()) {
            return fixture_run(app, &dir, m, own, installed, progress);
        }
    }
    let Some(staging) = gxio::update::staging_dir() else {
        return SelfUpdateOutcome::Failed(
            "The launcher could not create its staging folder, so it cannot download its update \
             safely."
                .to_string(),
        );
    };
    let download = |url: &str, dest: &std::path::Path, size: u64| {
        gxio::update::download_to_file(url, dest, size, &mut |done, _| progress(done))
    };
    with_production_seams(app, &download, |seams| {
        let pressed = SelfUpdateSeams { last_failed: &|| None, ..*seams };
        run_self_update(m, installed, rel::platform_token(), own_build(), &staging, std::process::id(), &pressed)
    })
}

/// THE FIXTURE'S WORLD, for the two places an update is DECIDED: the release verdict
/// (`cmd_release::build_release_state`) and the press (`release_apply`'s macOS arm). `None` in every
/// build but a fixture build run with `GX_SELFUPDATE_FIXTURE` set - and so in every packaged one.
///
/// GeneralsX @bugfix Claude 28/09/2026 (Codex rel0949-r2) WHY THE DECISION NEEDS IT NOW. The fixture used
/// to be read by the update that STARTED BY ITSELF, which took the fixture's document and identity at
/// its first line. Since 28/09/2026 only a press runs that update, and a press is reached through the
/// verdict: in the isolated, offline account the fixture exists for there is no published or cached
/// document, and a fixture build has no commit time, so the verdict named nothing, MainWeb never
/// handed over, and `release_apply` refused for want of a document before the fixture was ever read.
/// With this the verdict and the press judge the fixture's document against the fixture's identity,
/// exactly as the started update did - and everything after the decision stays production's.
#[cfg(gx_selfupdate_fixture)]
pub(crate) fn fixture_world() -> Option<(&'static gxcore::Manifest, OwnBuild<'static>)> {
    static FX: std::sync::OnceLock<Option<Fixture>> = std::sync::OnceLock::new();
    let fx = FX
        .get_or_init(|| {
            let text = std::fs::read_to_string(fixture_dir()?.join(FIXTURE_FILE)).ok()?;
            parse_fixture(&text).ok()
        })
        .as_ref()?;
    Some((&fx.manifest, OwnBuild { commit9: &fx.own_commit9, commit_epoch: Some(fx.own_epoch) }))
}

#[cfg(not(gx_selfupdate_fixture))]
pub(crate) fn fixture_world() -> Option<(&'static gxcore::Manifest, OwnBuild<'static>)> {
    None
}

fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn publish(app: &tauri::AppHandle, s: SelfUpdateStatusDto) {
    use tauri::Emitter;
    if let Ok(mut last) = SELF_UPDATE_STATUS.lock() {
        *last = Some(s.clone());
    }
    // Best effort, like every emit in this launcher: a page with no listener is normal.
    let _ = app.emit(SELF_UPDATE_EVENT, s);
}

/// The artifact that last failed to install, and when; nothing for a missing or unreadable file.
fn read_backoff() -> Option<(u64, String)> {
    rel::read_cached(SELF_UPDATE_BACKOFF, 4096).and_then(|(_, t)| parse_backoff(&t))
}

/// The swap lock production takes: [`SWAP_LOCK_FILE`] in the state directory, one file for every
/// launcher process of this account, taken without waiting ([`try_swap_lock`]).
fn take_swap_lock() -> Result<Option<SwapLock>, String> {
    let path = rel::cache_path(SWAP_LOCK_FILE)
        .ok_or("There is no per-user state directory to hold the launcher's swap lock.")?;
    try_swap_lock(&path)
}

/// Production's seams, around the `download` the caller chooses.
fn with_production_seams<R>(
    app: &tauri::AppHandle,
    download: &dyn Fn(&str, &std::path::Path, u64) -> Result<u64, String>,
    f: impl FnOnce(&SelfUpdateSeams<'_, std::sync::MutexGuard<'static, ()>>) -> R,
) -> R {
    let locate = gxio::update::locate_target;
    let hold = || wait_for_bundles(&BUNDLE_WORK, BUNDLE_WAIT, &|| std::thread::sleep(BUNDLE_POLL));
    let apply = |archive: &std::path::Path,
                 target: &gxio::update::Target,
                 commit9: &str|
     -> Result<(), SwapRefused> {
        let gxio::update::Target::MacApp(bundle) = target else {
            return Err(SwapRefused::Unavailable(
                "The launcher's own background update runs on macOS only.".to_string(),
            ));
        };
        let probe = |b: &std::path::Path| run_identify(&bundle_executable(b), PROBE_TIMEOUT);
        apply_launcher(
            archive,
            bundle,
            commit9,
            &take_swap_lock,
            &gxio::update::exchange_bundles,
            &probe,
        )
    };
    let record = write_launcher_record;
    let report = |phase: &str, name: &str| {
        eprintln!("[gx] self-update: {} {phase} {name}", now_secs());
        publish(
            app,
            SelfUpdateStatusDto {
                phase: phase.to_string(),
                detail: String::new(),
                name: name.to_string(),
            },
        );
    };
    let last_failed = read_backoff;
    let failed = |artifact: &str, at: u64| {
        // Best effort: a state directory that cannot be written costs the day's wait, and the next
        // launch tries this artifact again.
        let _ = rel::write_cache_atomic(SELF_UPDATE_BACKOFF, &backoff_line(at, artifact));
    };
    let now = now_secs;
    f(&SelfUpdateSeams {
        locate: &locate,
        download,
        hold_bundles: &hold,
        apply: &apply,
        record: &record,
        report: &report,
        last_failed: &last_failed,
        failed: &failed,
        now: &now,
    })
}

/// The record step `release_apply` performs, on the same rule: `install_record_target` puts the
/// record where a later swap of the bundle will not destroy it.
fn write_launcher_record(target: &gxio::update::Target, state: &gxcore::InstallState) -> bool {
    let (gxio::update::Target::MacApp(path) | gxio::update::Target::AppImage(path)) = target;
    match rel::install_record_target(Some(path)) {
        Some(at) => match rel::write_install_record(&at, &state.render()) {
            Ok(()) => true,
            Err(e) => {
                eprintln!(
                    "[gx] self-update: the record could not be written to {} ({e}); the launcher \
                     checks again at its next start",
                    at.display()
                );
                false
            }
        },
        None => {
            eprintln!(
                "[gx] self-update: there is nowhere to write the record; the launcher checks again \
                 at its next start"
            );
            false
        }
    }
}

/// The folder a fixture is read from - ONLY IN A FIXTURE BUILD (build_env.rs), which a build that
/// carries a commit time can never be. So no packaged launcher reads this variable, and its name is
/// not in a packaged binary at all.
#[cfg(gx_selfupdate_fixture)]
fn fixture_dir() -> Option<std::path::PathBuf> {
    std::env::var_os("GX_SELFUPDATE_FIXTURE").map(std::path::PathBuf::from)
}

/// A fixture build's run. The document and this binary's identity are the fixture's snapshot
/// (`fixture_world`, the one the verdict and the press judged) and the download is a copy out of the
/// fixture folder; the location, the check against the document's hash, the bundle lock, the
/// exchange, the new launcher's answer and the record are production's. That split is the point:
/// everything the swap can break is the real thing, and nothing is published or fetched to exercise it.
#[cfg(gx_selfupdate_fixture)]
fn fixture_run(
    app: &tauri::AppHandle,
    dir: &std::path::Path,
    m: &gxcore::Manifest,
    own: OwnBuild<'_>,
    installed: gxcore::InstallState,
    progress: &dyn Fn(u64),
) -> SelfUpdateOutcome {
    let Some(staging) = gxio::update::staging_dir() else {
        return SelfUpdateOutcome::Failed("The launcher could not create its staging folder.".to_string());
    };
    // The artifact the run chose, by the name at the end of its URL (`https://<base><name>`), out of
    // the fixture folder.
    let copy = |url: &str, dest: &std::path::Path, size: u64| -> Result<u64, String> {
        let source = dir.join(url.rsplit('/').next().unwrap_or_default());
        let got = std::fs::copy(&source, dest)
            .map_err(|e| format!("fixture: {}: {e}", source.display()))?;
        if got != size {
            let _ = std::fs::remove_file(dest);
            return Err(format!("fixture: {got} bytes, published as {size}"));
        }
        progress(got);
        Ok(got)
    };
    // The PRESSED run's seams, as `pressed_once` takes them: no day's backoff - the press is the retry.
    // No page reload after the swap any more: the press relaunches into the new bundle
    // (`restart_needed`), and a reload here would drop the page that is about to ask for it.
    with_production_seams(app, &copy, |seams| {
        let pressed = SelfUpdateSeams { last_failed: &|| None, ..*seams };
        run_self_update(m, installed, rel::platform_token(), own, &staging, std::process::id(), &pressed)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // build_env.rs, compiled here exactly as build.rs compiles it.
    mod build_env {
        include!("../build_env.rs");
    }

    /// Trimmed lines with the comment lines taken out, so prose ABOUT a directive can neither
    /// satisfy nor fail a pin on the directive.
    fn code_lines(src: &str, comment: &str) -> Vec<String> {
        src.lines()
            .map(str::trim)
            .filter(|l| !l.starts_with(comment))
            .map(str::to_string)
            .collect()
    }

    #[test]
    fn a_build_with_nothing_set_is_a_development_build_with_no_fixture() {
        let d = build_env::stamp_directives(None, None).expect("an ordinary build with nothing set");
        for must in [
            "cargo:rerun-if-env-changed=GX_COMMIT_EPOCH",
            "cargo:rerun-if-env-changed=GX_SELFUPDATE_FIXTURE_BUILD",
            "cargo:rustc-check-cfg=cfg(gx_selfupdate_fixture)",
        ] {
            assert!(
                d.iter().any(|l| l == must),
                "without `{must}` cargo keeps the first build's answer, or warns on the fixture's \
                 cfg: {d:?}"
            );
        }
        assert!(
            !d.iter().any(|l| l.starts_with("cargo:rustc-env=GX_COMMIT_EPOCH")),
            "nothing set must mean no commit time, which is what a development build is: {d:?}"
        );
        assert!(
            !d.iter().any(|l| l.starts_with("cargo:rustc-cfg=")),
            "nothing set must build no fixture in: {d:?}"
        );
        assert_eq!(
            build_env::stamp_directives(Some("  "), Some("")),
            Ok(d.clone()),
            "a blank value is not a value"
        );
    }

    #[test]
    fn the_packagers_commit_time_is_compiled_in_exactly_as_given() {
        let d = build_env::stamp_directives(Some(" 1789129912\n"), None)
            .expect("a commit time is an ordinary packaged build");
        assert!(
            d.iter().any(|l| l == "cargo:rustc-env=GX_COMMIT_EPOCH=1789129912"),
            "the packager's commit time must reach the binary, trimmed and otherwise unchanged: \
             {d:?}"
        );
        assert!(
            !d.iter().any(|l| l.starts_with("cargo:rustc-cfg=")),
            "a packaged build carries no fixture: {d:?}"
        );
        for wrong in ["2026-09-11", "1789129912.5", "-1", "17891299120000"] {
            let e = build_env::stamp_directives(Some(wrong), None)
                .expect_err("a value that is not a commit time must stop the build");
            assert!(e.contains("not a commit time"), "{wrong}: {e}");
        }
    }

    #[test]
    fn a_zero_commit_time_stops_the_build_rather_than_install_anything() {
        // "0" is a number of seconds and not a commit time. Compiled in, it would read as the oldest
        // build there is, and the rule would install whatever is published, downgrades included.
        for zero in ["0", "000", " 0 "] {
            let e = build_env::stamp_directives(Some(zero), None).expect_err(
                "a zero commit time must stop the build, not ship a launcher that installs anything",
            );
            assert!(e.contains("not a commit time"), "{zero:?}: {e}");
        }
    }

    #[test]
    fn a_packaged_build_can_never_carry_the_self_update_fixture() {
        let e = build_env::stamp_directives(Some("1789129912"), Some("1")).expect_err(
            "a build with a commit time is a packaged build, and it must refuse the fixture",
        );
        assert!(e.contains("must never honour the self-update fixture"), "{e}");

        let d = build_env::stamp_directives(None, Some("1")).expect("a fixture build has no commit time");
        assert!(d.iter().any(|l| l == "cargo:rustc-cfg=gx_selfupdate_fixture"), "{d:?}");
        assert!(!d.iter().any(|l| l.starts_with("cargo:rustc-env=GX_COMMIT_EPOCH")), "{d:?}");

        let e = build_env::stamp_directives(None, Some("yes")).expect_err("only 1 builds it in");
        assert!(e.contains("the only value it takes is 1"), "{e}");
    }

    #[test]
    fn build_rs_prints_what_build_env_decides_and_stops_on_a_refusal() {
        let code = code_lines(include_str!("../build.rs"), "//");
        for must in [
            "include!(\"build_env.rs\");",
            "let epoch = std::env::var(\"GX_COMMIT_EPOCH\").ok();",
            "let fixture = std::env::var(\"GX_SELFUPDATE_FIXTURE_BUILD\").ok();",
            "match stamp_directives(epoch.as_deref(), fixture.as_deref()) {",
            "Ok(lines) => lines.iter().for_each(|l| println!(\"{l}\")),",
            "Err(why) => panic!(\"{why}\"),",
        ] {
            assert!(
                code.iter().any(|l| l == must),
                "build.rs has no `{must}`, so the rule the tests above hold is not the rule the \
                 build runs"
            );
        }
        let stamp = code
            .iter()
            .position(|l| l.starts_with("match stamp_directives("))
            .expect("checked above");
        let tauri = code
            .iter()
            .position(|l| l == "tauri_build::build()")
            .expect("tauri_build::build() is gone from build.rs");
        assert!(stamp < tauri, "the stamp must be printed before tauri_build runs");
    }

    #[test]
    fn the_packager_compiles_in_the_commit_time_and_never_the_clock() {
        let mac = code_lines(
            include_str!("../../../build/launcher/package-launcher-macos.sh"),
            "#",
        );
        let set = mac
            .iter()
            .position(|l| l == "GX_COMMIT_EPOCH=\"$(git -C \"$REPO\" show -s --format=%ct HEAD)\"")
            .expect(
                "package-launcher-macos.sh does not set GX_COMMIT_EPOCH from the commit's own \
                 time, so every packaged launcher is a development build that never updates itself",
            );
        let export = mac
            .iter()
            .position(|l| l == "export GX_COMMIT_EPOCH")
            .expect("GX_COMMIT_EPOCH is set but never exported, so cargo never sees it");
        let build = mac
            .iter()
            .position(|l| l.contains("cargo tauri build"))
            .expect("the packager's build line is gone");
        assert!(
            set < export && export < build,
            "the commit time must be set, then exported, before cargo tauri build runs"
        );
        assert!(
            !mac.iter().any(|l| l.contains("date +%s")),
            "packaging wall-clock time must never stand in for the commit time"
        );
        assert!(
            !include_str!("../../../build/launcher/remote-build-linux.sh").contains("GX_COMMIT_EPOCH"),
            "the background update is macOS only, so the Linux build must not gain a commit time"
        );
    }

    // ---- the rule: is a newer launcher published? -------------------------------------------

    const PUBLISHED: &[u8] = b"the launcher the publisher signed";

    /// A launcher packaged at commit `aaaaaaaaa`, whose commit time is 1000.
    const PACKAGED: OwnBuild<'static> = OwnBuild { commit9: "aaaaaaaaa", commit_epoch: Some(1_000) };

    fn launcher(commit: &str) -> gxcore::Artifact {
        gxcore::Artifact {
            platform: "macos".into(),
            name: format!("GeneralsXLauncher-macos-{commit}.tar.gz"),
            hash: gxcore::artifact_hash(PUBLISHED),
            size: PUBLISHED.len() as u64,
        }
    }

    fn doc(ordinal: u64, artifacts: Vec<gxcore::Artifact>) -> gxcore::Manifest {
        gxcore::Manifest {
            version: 2,
            ordinal,
            minimum: 0,
            rank: 1,
            serial: None,
            sha: String::new(),
            relays: Vec::new(),
            base: "dl.example/".into(),
            artifacts,
        }
    }

    #[test]
    fn a_development_build_never_replaces_itself() {
        let dev = OwnBuild { commit9: "aaaaaaaaa", commit_epoch: None };
        match plan_self_update(&doc(2_000, vec![launcher("bbbbbbbbb")]), "macos", dev) {
            SelfUpdatePlan::Nothing(why) => assert!(
                why.contains("development build"),
                "a development build must be refused as one, got {why:?}"
            ),
            other => panic!("a build the packager did not make replaced itself: {other:?}"),
        }
    }

    #[test]
    fn a_launcher_published_after_this_build_is_installed() {
        let row = launcher("bbbbbbbbb");
        assert_eq!(
            plan_self_update(&doc(1_001, vec![row.clone()]), "macos", PACKAGED),
            SelfUpdatePlan::Install(row),
            "a release one second newer than this binary's commit names a newer launcher"
        );
    }

    #[test]
    fn a_launcher_published_before_or_with_this_build_is_never_installed() {
        for ordinal in [999, 1_000] {
            match plan_self_update(&doc(ordinal, vec![launcher("bbbbbbbbb")]), "macos", PACKAGED) {
                SelfUpdatePlan::Nothing(why) => {
                    assert!(
                        why.contains("is not newer than this one"),
                        "#{ordinal}: the refusal must say the published launcher is not newer, got \
                         {why:?}"
                    )
                }
                other => panic!(
                    "a launcher published at #{ordinal}, not after this build's #1000, would \
                     replace it: {other:?}"
                ),
            }
        }
    }

    #[test]
    fn the_published_launcher_is_this_launcher_whatever_the_ordinal() {
        let row = launcher("aaaaaaaaa");
        for ordinal in [999, 1_000, 5_000] {
            assert_eq!(
                plan_self_update(&doc(ordinal, vec![row.clone()]), "macos", PACKAGED),
                SelfUpdatePlan::Current(row.clone()),
                "#{ordinal}: this binary's own commit is published, so nothing is fetched"
            );
        }
    }

    #[test]
    fn only_this_platforms_launcher_row_is_ever_installed() {
        let mut engine = launcher("bbbbbbbbb");
        engine.name = "GeneralsXZH-macos-arm64-bbbbbbbbb.zip".into();
        let mut overlay = launcher("bbbbbbbbb");
        overlay.platform = "any".into();
        let mut linux = launcher("bbbbbbbbb");
        linux.platform = "linux".into();
        linux.name = "GeneralsXLauncher-linux-x86_64-bbbbbbbbb.AppImage".into();

        let others = vec![engine.clone(), overlay.clone(), linux.clone()];
        match plan_self_update(&doc(2_000, others), "macos", PACKAGED) {
            SelfUpdatePlan::Nothing(why) => assert!(
                why.contains("names no macos launcher"),
                "with no macos launcher row published the refusal must say so, got {why:?}"
            ),
            other => panic!(
                "an engine payload, an `any` overlay or another platform's launcher was taken for \
                 this launcher: {other:?}"
            ),
        }

        let ours = launcher("bbbbbbbbb");
        assert_eq!(
            plan_self_update(&doc(2_000, vec![engine, overlay, ours.clone(), linux]), "macos", PACKAGED),
            SelfUpdatePlan::Install(ours),
            "with ours among them, ours - wherever it is published"
        );
    }

    // ---- the order: plan, backoff, locate, download, verify, hold, swap, record --------------

    /// A staging directory of this test's own, removed on drop.
    struct Scratch(std::path::PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Scratch {
            use std::sync::atomic::{AtomicU32, Ordering};
            static N: AtomicU32 = AtomicU32::new(0);
            let p = std::env::temp_dir().join(format!(
                "gx-selfupdate-{tag}-{}-{}",
                std::process::id(),
                N.fetch_add(1, Ordering::Relaxed)
            ));
            std::fs::create_dir_all(&p).expect("a scratch staging directory");
            Scratch(p)
        }

        fn leftovers(&self) -> Vec<String> {
            let mut v: Vec<String> = std::fs::read_dir(&self.0)
                .expect("the scratch directory is readable")
                .flatten()
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect();
            v.sort();
            v
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    /// The pid a run stages under. Deliberately not this process's, so the staged name can only
    /// have come from the argument.
    const PID: u32 = 4242;

    /// The clock every run below reads, unless it says otherwise.
    const NOW: u64 = 1_789_129_912;

    /// What the seams saw, in the order they saw it.
    #[derive(Default)]
    struct Probe {
        log: std::cell::RefCell<Vec<&'static str>>,
        url: std::cell::RefCell<String>,
        downloaded_to: std::cell::RefCell<Option<std::path::PathBuf>>,
        held_at_apply: std::cell::Cell<Option<bool>>,
        held_at_record: std::cell::Cell<Option<bool>>,
        recorded: std::cell::RefCell<Option<gxcore::InstallState>>,
        /// The commit the new launcher was held to.
        commit: std::cell::RefCell<String>,
        /// What the run remembered as failed: (artifact, when).
        failed: std::cell::RefCell<Option<(String, u64)>>,
    }

    /// How the world answers one run.
    struct World<'a> {
        locatable: bool,
        /// True: this copy locates as an AppImage, which a macOS row cannot rebuild.
        appimage: bool,
        body: Result<&'a [u8], &'a str>,
        /// False: an engine install holds the bundles for the whole wait.
        bundles_free: bool,
        /// What putting the new launcher in place, and hearing it answer, comes to.
        swap: Result<(), SwapRefused>,
        last_failed: Option<(u64, String)>,
        now: u64,
    }

    fn ok() -> World<'static> {
        World {
            locatable: true,
            appimage: false,
            body: Ok(PUBLISHED),
            bundles_free: true,
            swap: Ok(()),
            last_failed: None,
            now: NOW,
        }
    }

    /// One run of the job against `world`, with a bundle lock this test owns. Never the real one.
    fn run_in(
        m: &gxcore::Manifest,
        own: OwnBuild<'_>,
        world: World<'_>,
        staging: &std::path::Path,
    ) -> (SelfUpdateOutcome, Probe) {
        let p = Probe::default();
        let bundles = std::sync::Mutex::new(());
        let out = {
            let locate = || -> Result<gxio::update::Target, String> {
                p.log.borrow_mut().push("locate");
                if !world.locatable {
                    return Err(
                        "The launcher is running from the read-only copy macOS makes.".to_string()
                    );
                }
                Ok(if world.appimage {
                    gxio::update::Target::AppImage(std::path::PathBuf::from(
                        "/opt/GeneralsX/GeneralsXLauncher.AppImage",
                    ))
                } else {
                    gxio::update::Target::MacApp(std::path::PathBuf::from(
                        "/Applications/GeneralsX Launcher.app",
                    ))
                })
            };
            let download = |url: &str, dest: &std::path::Path, _size: u64| -> Result<u64, String> {
                p.log.borrow_mut().push("download");
                *p.url.borrow_mut() = url.to_string();
                *p.downloaded_to.borrow_mut() = Some(dest.to_path_buf());
                let body = world.body.map_err(str::to_string)?;
                std::fs::write(dest, body).map_err(|e| e.to_string())?;
                Ok(body.len() as u64)
            };
            let hold = || {
                p.log.borrow_mut().push("hold");
                if world.bundles_free {
                    Some(bundles.lock().unwrap_or_else(|e| e.into_inner()))
                } else {
                    None
                }
            };
            let apply = |staged: &std::path::Path,
                         _: &gxio::update::Target,
                         commit9: &str|
             -> Result<(), SwapRefused> {
                p.log.borrow_mut().push("apply");
                assert!(staged.is_file(), "the swap was handed a file that is not there");
                p.held_at_apply.set(Some(bundles.try_lock().is_err()));
                *p.commit.borrow_mut() = commit9.to_string();
                world.swap.clone()
            };
            let record = |_: &gxio::update::Target, st: &gxcore::InstallState| -> bool {
                p.log.borrow_mut().push("record");
                p.held_at_record.set(Some(bundles.try_lock().is_err()));
                *p.recorded.borrow_mut() = Some(st.clone());
                true
            };
            let report = |_: &str, _: &str| {};
            let last_failed = || world.last_failed.clone();
            let failed = |name: &str, at: u64| {
                *p.failed.borrow_mut() = Some((name.to_string(), at));
            };
            let now = || world.now;
            let seams = SelfUpdateSeams {
                locate: &locate,
                download: &download,
                hold_bundles: &hold,
                apply: &apply,
                record: &record,
                report: &report,
                last_failed: &last_failed,
                failed: &failed,
                now: &now,
            };
            run_self_update(m, gxcore::InstallState::default(), "macos", own, staging, PID, &seams)
        };
        (out, p)
    }

    const NEWER: &str = "GeneralsXLauncher-macos-bbbbbbbbb.tar.gz";

    #[test]
    fn a_newer_launcher_is_located_fetched_verified_held_swapped_and_recorded_in_that_order() {
        let s = Scratch::new("happy");
        let m = doc(2_000, vec![launcher("bbbbbbbbb")]);
        let (out, p) = run_in(&m, PACKAGED, ok(), &s.0);
        assert_eq!(out, SelfUpdateOutcome::Installed { name: NEWER.into(), recorded: true });
        assert_eq!(*p.log.borrow(), ["locate", "download", "hold", "apply", "record"]);
        assert_eq!(*p.url.borrow(), format!("https://dl.example/{NEWER}"));
        assert_eq!(p.held_at_apply.get(), Some(true), "the swap ran without the bundle lock");
        assert_eq!(
            p.held_at_record.get(),
            Some(false),
            "the bundle lock is for the extract, the exchange and the answer; it was still held for \
             the record"
        );
        assert_eq!(
            *p.commit.borrow(),
            "bbbbbbbbb",
            "the new launcher must be held to the commit its release row is named after"
        );
        let st = p.recorded.borrow().clone().expect("the swap was recorded");
        assert!(
            matches!(gxcore::decide(&m, &st, "macos"), gxcore::Verdict::UpToDate),
            "the record must make the same document read as up to date, or the same update is \
             offered again: {st:?}"
        );
        assert_eq!(*p.failed.borrow(), None, "an installed launcher is not a failure");
        assert!(s.leftovers().is_empty(), "the staged download outlived the swap: {:?}", s.leftovers());
    }

    #[test]
    fn a_download_that_does_not_match_the_signature_is_never_swapped() {
        let s = Scratch::new("mismatch");
        let world = World { body: Ok(b"bytes the publisher never signed"), ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(
            matches!(&out, SelfUpdateOutcome::Failed(e) if e.contains("does not match the signed release")),
            "a download that does not match the signed hash must fail the run: {out:?}"
        );
        assert_eq!(*p.log.borrow(), ["locate", "download"], "something ran after a failed check");
        assert!(s.leftovers().is_empty(), "the refused file must be gone: {:?}", s.leftovers());
    }

    #[test]
    fn a_partial_download_is_never_swapped() {
        let s = Scratch::new("partial");
        let world = World { body: Ok(&PUBLISHED[..PUBLISHED.len() / 2]), ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(matches!(out, SelfUpdateOutcome::Failed(_)), "a partial download must fail the run: {out:?}");
        assert_eq!(*p.log.borrow(), ["locate", "download"]);
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn a_failed_download_swaps_nothing_and_leaves_nothing() {
        let s = Scratch::new("dead-link");
        let world = World { body: Err("The download did not finish."), ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert_eq!(out, SelfUpdateOutcome::Failed("The download did not finish.".into()));
        assert_eq!(*p.log.borrow(), ["locate", "download"]);
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn the_swap_defers_rather_than_land_inside_an_engine_install() {
        let s = Scratch::new("engine-busy");
        let world = World { bundles_free: false, ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(
            matches!(&out, SelfUpdateOutcome::Deferred(why) if why.contains("engine install")),
            "an engine install holding the bundles must defer the swap: {out:?}"
        );
        assert_eq!(*p.log.borrow(), ["locate", "download", "hold"], "the swap ran under an engine install");
        assert_eq!(
            *p.failed.borrow(),
            None,
            "an engine install in the way is not the artifact failing; it is tried at the next launch"
        );
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn a_swap_another_launcher_process_is_making_waits_for_the_next_launch() {
        let s = Scratch::new("swap-busy");
        let busy = "Another GeneralsX launcher is replacing the launcher right now.";
        let world = World { swap: Err(SwapRefused::Busy(busy.into())), ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert_eq!(
            out,
            SelfUpdateOutcome::Deferred(busy.into()),
            "a swap another launcher process is making must wait for the next launch"
        );
        assert_eq!(*p.log.borrow(), ["locate", "download", "hold", "apply"]);
        assert_eq!(
            *p.failed.borrow(),
            None,
            "another launcher replacing itself is not this artifact failing, so it must not wait a day"
        );
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn a_swap_that_fails_records_nothing_and_leaves_nothing_staged() {
        let s = Scratch::new("swap-refused");
        for (swap, want) in [
            (
                SwapRefused::Failed("It did not answer.".into()),
                SelfUpdateOutcome::Failed("It did not answer.".into()),
            ),
            (
                SwapRefused::Stuck("It could not be put back.".into()),
                SelfUpdateOutcome::Failed("It could not be put back.".into()),
            ),
            (
                SwapRefused::Unavailable("No exchange here.".into()),
                SelfUpdateOutcome::Unavailable("No exchange here.".into()),
            ),
        ] {
            let world = World { swap: Err(swap), ..ok() };
            let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
            assert_eq!(out, want);
            assert_eq!(
                *p.log.borrow(),
                ["locate", "download", "hold", "apply"],
                "a launcher that was not kept was recorded"
            );
            assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
        }
    }

    #[test]
    fn a_copy_that_cannot_replace_itself_says_so_only_when_a_newer_launcher_is_published() {
        let s = Scratch::new("translocated");
        let world = World { locatable: false, ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(
            matches!(&out, SelfUpdateOutcome::Unavailable(why) if why.contains("read-only copy")),
            "a copy that cannot be replaced must say so when a newer launcher exists: {out:?}"
        );
        assert_eq!(*p.log.borrow(), ["locate"], "something was fetched for a copy that cannot be replaced");

        let world = World { locatable: false, ..ok() };
        let (out, p) = run_in(&doc(900, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(matches!(out, SelfUpdateOutcome::Nothing(_)), "with nothing newer published the answer is nothing: {out:?}");
        assert!(
            p.log.borrow().is_empty(),
            "with nothing newer published, nothing - not even the location - is looked at: {:?}",
            p.log.borrow()
        );
    }

    #[test]
    fn only_a_launcher_this_copy_can_be_rebuilt_from_is_fetched() {
        // A macOS row, and a running form that is an AppImage: the selector the irreversible swap
        // uses everywhere refuses the pair, so nothing is downloaded for a swap that cannot happen.
        let s = Scratch::new("wrong-form");
        let world = World { appimage: true, ..ok() };
        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, world, &s.0);
        assert!(
            matches!(&out, SelfUpdateOutcome::Nothing(why) if why.contains("can be replaced from")),
            "a row this running form cannot be rebuilt from must be refused before anything is \
             fetched: {out:?}"
        );
        assert_eq!(*p.log.borrow(), ["locate"], "something was fetched that could never be installed");
    }

    #[test]
    fn the_published_launcher_is_recorded_and_nothing_is_fetched() {
        let s = Scratch::new("current");
        let m = doc(2_000, vec![launcher("aaaaaaaaa")]);
        let (out, p) = run_in(&m, PACKAGED, ok(), &s.0);
        assert_eq!(out, SelfUpdateOutcome::Current { recorded: true });
        assert_eq!(*p.log.borrow(), ["locate", "record"], "the running build was fetched again");
        let st = p.recorded.borrow().clone().expect("the record was brought up to date");
        assert!(
            matches!(gxcore::decide(&m, &st, "macos"), gxcore::Verdict::UpToDate),
            "the home screen's verdict must agree with this binary on its next read: {st:?}"
        );

        let (out, p) = run_in(&m, PACKAGED, World { locatable: false, ..ok() }, &s.0);
        assert_eq!(out, SelfUpdateOutcome::Current { recorded: false });
        assert_eq!(*p.log.borrow(), ["locate"], "a record was written with nowhere safe to put it");
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn a_download_in_flight_from_another_launcher_is_never_touched() {
        let s = Scratch::new("two-launchers");
        let shared = s.0.join(NEWER);
        let other = s.0.join(format!("{}.{NEWER}", PID + 1));
        std::fs::write(&shared, b"release_apply's download, mid-transfer").unwrap();
        std::fs::write(&other, b"another launcher's download, mid-transfer").unwrap();

        let (out, p) = run_in(&doc(2_000, vec![launcher("bbbbbbbbb")]), PACKAGED, ok(), &s.0);
        assert!(matches!(out, SelfUpdateOutcome::Installed { .. }), "the run itself must still install: {out:?}");
        assert_eq!(
            p.downloaded_to.borrow().clone(),
            Some(s.0.join(format!("{PID}.{NEWER}"))),
            "this run must stage under its own pid"
        );
        assert_eq!(std::fs::read(&shared).unwrap(), b"release_apply's download, mid-transfer");
        assert_eq!(std::fs::read(&other).unwrap(), b"another launcher's download, mid-transfer");
        assert_eq!(
            s.leftovers(),
            [format!("{}.{NEWER}", PID + 1), NEWER.to_string()],
            "only this run's own staged file may be gone"
        );
    }

    #[test]
    fn a_launcher_that_failed_to_install_is_left_alone_for_a_day() {
        let s = Scratch::new("backoff");
        let m = doc(2_000, vec![launcher("bbbbbbbbb")]);

        let world = World { swap: Err(SwapRefused::Failed("It did not answer.".into())), ..ok() };
        let (out, p) = run_in(&m, PACKAGED, world, &s.0);
        assert!(matches!(out, SelfUpdateOutcome::Failed(_)), "{out:?}");
        assert_eq!(
            p.failed.borrow().clone(),
            Some((NEWER.to_string(), NOW)),
            "a failed install must be remembered, or the next launch downloads it again"
        );

        let world = World { last_failed: Some((NOW, NEWER.into())), now: NOW + RETRY_AFTER_SECS - 1, ..ok() };
        let (out, p) = run_in(&m, PACKAGED, world, &s.0);
        assert!(
            matches!(&out, SelfUpdateOutcome::Nothing(why) if why.contains("tries it again")),
            "the same artifact must be left alone for a day after it failed: {out:?}"
        );
        assert!(
            p.log.borrow().is_empty(),
            "inside the day nothing may be located, let alone downloaded: {:?}",
            p.log.borrow()
        );

        let world = World { last_failed: Some((NOW, NEWER.into())), now: NOW + RETRY_AFTER_SECS, ..ok() };
        let (out, _) = run_in(&m, PACKAGED, world, &s.0);
        assert!(matches!(out, SelfUpdateOutcome::Installed { .. }), "after a day it is tried again: {out:?}");

        let (_, p) = run_in(&m, PACKAGED, World { locatable: false, ..ok() }, &s.0);
        assert_eq!(
            p.failed.borrow().clone(),
            Some((NEWER.to_string(), NOW)),
            "a copy that cannot install it is remembered the same way"
        );
        assert!(s.leftovers().is_empty(), "{:?}", s.leftovers());
    }

    #[test]
    fn a_newer_launcher_is_tried_at_once_after_an_older_one_failed() {
        let s = Scratch::new("backoff-newer");
        let m = doc(2_000, vec![launcher("bbbbbbbbb")]);
        let older = "GeneralsXLauncher-macos-0123456ab.tar.gz".to_string();
        let world = World { last_failed: Some((NOW, older)), now: NOW + 60, ..ok() };
        let (out, p) = run_in(&m, PACKAGED, world, &s.0);
        assert!(
            matches!(out, SelfUpdateOutcome::Installed { .. }),
            "a launcher other than the one that failed must be tried at once: {out:?}"
        );
        assert_eq!(p.log.borrow()[..2], ["locate", "download"]);

        let world = World { last_failed: Some((NOW + 3_600, NEWER.into())), ..ok() };
        let (out, _) = run_in(&m, PACKAGED, world, &s.0);
        assert!(
            matches!(out, SelfUpdateOutcome::Installed { .. }),
            "a failure stamped after now is a clock that moved, and must hold nothing back: {out:?}"
        );
    }

    #[test]
    fn every_outcome_reaches_the_home_screen_under_the_keys_and_phases_it_reads() {
        let v = serde_json::to_value(
            SelfUpdateOutcome::Installed { name: "n".into(), recorded: true }.status(),
        )
        .expect("the status serialises");
        let mut keys: Vec<&str> = v.as_object().expect("an object").keys().map(String::as_str).collect();
        keys.sort();
        assert_eq!(keys, ["detail", "name", "phase"], "MainWeb reads u.phase, u.detail and u.name");

        let all = [
            SelfUpdateOutcome::Nothing("n".into()),
            SelfUpdateOutcome::Current { recorded: true },
            SelfUpdateOutcome::Unavailable("u".into()),
            SelfUpdateOutcome::Deferred("d".into()),
            SelfUpdateOutcome::Failed("f".into()),
            SelfUpdateOutcome::Installed { name: NEWER.into(), recorded: false },
        ];
        let phases: Vec<String> = all.iter().map(|o| o.status().phase).collect();
        assert_eq!(phases, ["nothing", "current", "unavailable", "deferred", "failed", "installed"]);
        for (o, detail) in all[2..5].iter().zip(["u", "d", "f"]) {
            assert_eq!(o.status().detail, detail, "the sentence MainWeb shows is the outcome's own");
        }
        let installed = all[5].status();
        assert_eq!(installed.name, NEWER);
        assert!(installed.detail.contains("next time you open the launcher"), "{installed:?}");
    }

    // ---- the swap: one exchange, an answer, and only then the old launcher goes --------------

    /// The installed launcher and the one staged beside it, each with a marker to tell them apart.
    struct Bundles {
        _dir: Scratch,
        app: std::path::PathBuf,
        fresh: std::path::PathBuf,
    }

    fn bundles(tag: &str) -> Bundles {
        let dir = Scratch::new(tag);
        let app = dir.0.join("GeneralsX Launcher.app");
        let fresh = dir.0.join(".gxupdate-test").join("GeneralsX Launcher.app");
        for (b, which) in [(&app, "old"), (&fresh, "new")] {
            let macos = b.join("Contents").join("MacOS");
            std::fs::create_dir_all(&macos).expect("a stand-in bundle");
            std::fs::write(macos.join("marker"), which).expect("its marker");
        }
        Bundles { _dir: dir, app, fresh }
    }

    fn marker(bundle: &std::path::Path) -> Option<String> {
        std::fs::read_to_string(bundle.join("Contents").join("MacOS").join("marker")).ok()
    }

    /// A stand-in for `renamex_np(RENAME_SWAP)`: three renames the test's own filesystem certainly
    /// makes, so the exchange really happens whatever volume the test runs on.
    fn exchanged(a: &std::path::Path, b: &std::path::Path) -> Result<(), String> {
        let aside = b.with_extension("exchanging");
        std::fs::rename(b, &aside).map_err(|e| e.to_string())?;
        std::fs::rename(a, b).map_err(|e| e.to_string())?;
        std::fs::rename(&aside, a).map_err(|e| e.to_string())
    }

    /// The one line a launcher prints when it is asked who it is.
    fn identity(commit: &str, epoch: u64) -> String {
        format!("{{\"gxidentify\":1,\"commit\":\"{commit}\",\"commit_epoch\":{epoch}}}\n")
    }

    fn answers(commit: &str, epoch: u64) -> ProbeRun {
        ProbeRun::Exited { code: Some(0), stdout: identity(commit, epoch) }
    }

    #[test]
    fn a_new_launcher_that_answers_with_its_commit_is_kept_and_the_old_one_let_go() {
        let b = bundles("probe-pass");
        let seen = std::cell::RefCell::new(None);
        let probe = |at: &std::path::Path| {
            *seen.borrow_mut() = Some((marker(at), marker(&b.fresh)));
            answers("bbbbbbbbb", 2_000)
        };
        assert_eq!(swap_checked(&b.fresh, &b.app, &exchanged, &probe, "bbbbbbbbb"), Ok(()));
        assert_eq!(
            seen.borrow().clone(),
            Some((Some("new".to_string()), Some("old".to_string()))),
            "the NEW launcher must be started where the player keeps it, while the old one still \
             exists beside it"
        );
        assert_eq!(marker(&b.app).as_deref(), Some("new"));
        assert!(!b.fresh.exists(), "once the new launcher has answered, the old one must be let go");
    }

    #[test]
    fn a_new_launcher_that_names_another_commit_is_exchanged_back() {
        let b = bundles("probe-commit");
        let out = swap_checked(&b.fresh, &b.app, &exchanged, &|_: &std::path::Path| answers("ccccccccc", 2_000), "bbbbbbbbb");
        assert!(
            matches!(&out, Err(SwapRefused::Failed(why)) if why.contains("not the `bbbbbbbbb`")),
            "a launcher that is not the release's must not be kept: {out:?}"
        );
        assert_eq!(marker(&b.app).as_deref(), Some("old"), "the launcher the player had must be back in place");
        assert!(!b.fresh.exists(), "the refused launcher must be removed");
    }

    #[test]
    fn a_new_launcher_that_does_not_exit_cleanly_is_exchanged_back() {
        for run in [
            ProbeRun::Exited { code: Some(3), stdout: identity("bbbbbbbbb", 2_000) },
            ProbeRun::Exited { code: None, stdout: identity("bbbbbbbbb", 2_000) },
            ProbeRun::NotStarted("Bad CPU type in executable".into()),
        ] {
            let b = bundles("probe-exit");
            let out = swap_checked(&b.fresh, &b.app, &exchanged, &|_: &std::path::Path| run.clone(), "bbbbbbbbb");
            assert!(
                matches!(out, Err(SwapRefused::Failed(_))),
                "{run:?} is not an answer, whatever it printed: {out:?}"
            );
            assert_eq!(marker(&b.app).as_deref(), Some("old"), "{run:?}: the launcher the player had must be back");
            assert!(!b.fresh.exists(), "{run:?}: the refused launcher must be removed");
        }
    }

    #[test]
    fn a_new_launcher_that_does_not_answer_in_time_is_exchanged_back() {
        let b = bundles("probe-timeout");
        let out = swap_checked(&b.fresh, &b.app, &exchanged, &|_: &std::path::Path| ProbeRun::TimedOut, "bbbbbbbbb");
        assert!(
            matches!(&out, Err(SwapRefused::Failed(why)) if why.contains("did not answer")),
            "a launcher that hangs must not be kept: {out:?}"
        );
        assert_eq!(marker(&b.app).as_deref(), Some("old"), "the launcher the player had must be back in place");
        assert!(!b.fresh.exists(), "the refused launcher must be removed");
    }

    #[test]
    fn a_new_launcher_with_no_commit_time_is_exchanged_back() {
        let b = bundles("probe-dev");
        let out = swap_checked(&b.fresh, &b.app, &exchanged, &|_: &std::path::Path| answers("bbbbbbbbb", 0), "bbbbbbbbb");
        assert!(
            matches!(&out, Err(SwapRefused::Failed(why)) if why.contains("no commit time")),
            "a development build must not be kept - it would never update itself again: {out:?}"
        );
        assert_eq!(marker(&b.app).as_deref(), Some("old"));
    }

    #[test]
    fn a_previous_launcher_that_cannot_be_put_back_stays_in_the_stage_and_is_named() {
        let b = bundles("probe-stuck");
        let tries = std::cell::Cell::new(0u32);
        let only_once = |x: &std::path::Path, y: &std::path::Path| {
            tries.set(tries.get() + 1);
            if tries.get() == 1 {
                exchanged(x, y)
            } else {
                Err("the volume refused the second exchange".to_string())
            }
        };
        let out = swap_checked(&b.fresh, &b.app, &only_once, &|_: &std::path::Path| ProbeRun::TimedOut, "bbbbbbbbb");
        let at = b.fresh.display().to_string();
        assert!(
            matches!(&out, Err(SwapRefused::Stuck(why)) if why.contains(&at)),
            "the sentence must say where the previous launcher is: {out:?}"
        );
        assert_eq!(
            marker(&b.fresh).as_deref(),
            Some("old"),
            "the previous launcher is the only copy left, and the swap must not delete it"
        );
        assert_eq!(marker(&b.app).as_deref(), Some("new"));
    }

    #[test]
    fn an_exchange_that_is_refused_moves_nothing_and_starts_nothing() {
        let b = bundles("exchange-refused");
        let started = std::cell::Cell::new(false);
        let refuse = |_: &std::path::Path, _: &std::path::Path| -> Result<(), String> {
            Err("This volume cannot exchange two names in one step.".to_string())
        };
        let probe = |_: &std::path::Path| {
            started.set(true);
            ProbeRun::TimedOut
        };
        let out = swap_checked(&b.fresh, &b.app, &refuse, &probe, "bbbbbbbbb");
        assert!(
            matches!(&out, Err(SwapRefused::Unavailable(_))),
            "with no exchange the update is unavailable here, not failed: {out:?}"
        );
        assert!(!started.get(), "nothing was exchanged, so nothing may be started");
        assert_eq!(
            (marker(&b.app).as_deref(), marker(&b.fresh).as_deref()),
            (Some("old"), Some("new")),
            "nothing may move when the exchange is refused"
        );
    }

    // ---- the bundle lock ------------------------------------------------------------------------

    #[test]
    fn the_bundles_are_given_up_on_rather_than_shared() {
        let bundles = std::sync::Mutex::new(());
        let tick = || std::thread::sleep(std::time::Duration::from_millis(1));
        {
            let _an_engine_install = bundles.lock().unwrap();
            assert!(
                wait_for_bundles(&bundles, std::time::Duration::from_millis(20), &tick).is_none(),
                "the launcher's swap got the bundles while an engine install held them"
            );
        }
        assert!(
            wait_for_bundles(&bundles, std::time::Duration::from_millis(20), &tick).is_some(),
            "free bundles must be taken"
        );
    }

    #[test]
    fn an_install_that_finishes_inside_the_wait_is_waited_for() {
        let bundles = std::sync::Mutex::new(());
        let install = std::cell::RefCell::new(Some(bundles.lock().unwrap()));
        let pauses = std::cell::Cell::new(0u32);
        let the_install_finishes = || {
            pauses.set(pauses.get() + 1);
            install.borrow_mut().take();
        };
        assert!(
            wait_for_bundles(&bundles, std::time::Duration::from_secs(5), &the_install_finishes)
                .is_some(),
            "an install that finished during the wait must be waited for, not turned into a \
             deferral to the next launch"
        );
        assert_eq!(pauses.get(), 1, "one pause, then the bundles");
    }

    #[test]
    fn a_poisoned_bundle_lock_is_still_taken() {
        let bundles = std::sync::Mutex::new(());
        std::thread::scope(|s| {
            let _ = s
                .spawn(|| {
                    let _held = bundles.lock().unwrap();
                    panic!("an engine install panicked while it held the bundles");
                })
                .join();
        });
        assert!(bundles.is_poisoned(), "this test's setup did not poison the lock");
        drop(lock_bundles(&bundles));
        assert!(
            wait_for_bundles(&bundles, std::time::Duration::ZERO, &|| {}).is_some(),
            "one panicked install must not stop the launcher updating itself for good"
        );
    }

    #[test]
    fn an_engine_install_takes_the_bundles_right_after_its_claim() {
        let src = include_str!("cmd_setup.rs");
        let start = src.find("pub(crate) fn install_engine_reporting(").expect("the engine install is gone");
        let rest = &src[start..];
        let body: Vec<&str> = rest[..rest.find("\n}\n").expect("it closes in column 0")]
            .lines()
            .map(str::trim)
            .filter(|l| !l.starts_with("//"))
            .collect();
        let at = |needle: &str| body.iter().position(|l| l.contains(needle));
        let claim = at("let _install = claim_engine_install()?;").expect("the install claim is gone");
        let bundles = at(&format!("let _bundles = crate::self_update::{}();", "claim_bundle_work"))
            .expect(
                "setup_install_engine never holds the bundle lock (or drops it on the spot), so the \
                 launcher's own swap can land in the middle of an engine install",
            );
        let install = at("gxio::engine::install_engine(").expect("the install itself is gone");
        assert!(
            claim < bundles && body[claim + 1..bundles].iter().all(|l| l.is_empty()),
            "the bundle lock must be the next thing after the claim - the order PLAY's closer takes \
             them in: {:?}",
            &body[claim..=bundles.max(claim)]
        );
        assert!(bundles < install, "and it must be held before the install begins");
    }

    /// `line` with the contents of its string and char literals taken out, so a needle a test
    /// SPELLS never counts as the test reaching for the lock. Line by line: a literal that runs on
    /// to the next line leaves that line looking like code, which can only make the guard below
    /// stricter, never blinder.
    fn outside_literals(line: &str) -> String {
        let c: Vec<char> = line.chars().collect();
        let (mut out, mut i, mut in_str) = (String::new(), 0, false);
        while i < c.len() {
            if in_str {
                match c[i] {
                    '\\' => i += 1,
                    '"' => in_str = false,
                    _ => {}
                }
            } else if c[i] == '"' {
                in_str = true;
            } else if c[i] == '\'' && c.get(i + 1) == Some(&'"') && c.get(i + 2) == Some(&'\'') {
                i += 2;
            } else if c[i] == '\'' && c.get(i + 1) == Some(&'\\') && c.get(i + 3) == Some(&'\'') {
                i += 3;
            } else {
                out.push(c[i]);
            }
            i += 1;
        }
        out
    }

    /// The crate's own source folder, as it is on disk when the tests run.
    fn crate_src() -> std::path::PathBuf {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src")
    }

    /// Every `.rs` file under `dir`, at any depth: its path under `dir`, with `/` between folders,
    /// and its text. Sorted by path.
    fn rust_files(dir: &std::path::Path) -> Vec<(String, String)> {
        let mut files = Vec::new();
        let mut folders = vec![dir.to_path_buf()];
        while let Some(folder) = folders.pop() {
            for entry in std::fs::read_dir(&folder).expect("a source folder can be read").flatten() {
                let p = entry.path();
                if p.is_dir() {
                    folders.push(p);
                } else if p.extension().is_some_and(|e| e == "rs") {
                    let name = p.strip_prefix(dir).expect("it is under dir").to_string_lossy();
                    let text = std::fs::read_to_string(&p).expect("a source file can be read");
                    files.push((name.replace('\\', "/"), text));
                }
            }
        }
        files.sort();
        files
    }

    /// THE TEST CODE OF EVERY FILE UNDER `dir` THAT HAS ANY: the file, and its text from its first
    /// test module to its end. A test module is a `mod` under a `#[cfg(...)]` that names `test`;
    /// one declared as `mod name;` lives in a file of its own, and that file is test code whole.
    /// READ FROM DISK WHEN THE TESTS RUN, never from a list, so a file added to the crate later is
    /// read without anybody remembering to add it.
    fn test_code(dir: &std::path::Path) -> Vec<(String, String)> {
        let files = rust_files(dir);
        let mut own_files: Vec<String> = Vec::new();
        let mut starts: Vec<Option<usize>> = Vec::new();
        for (_, text) in &files {
            let lines: Vec<&str> = text.split_inclusive('\n').collect();
            let (mut start, mut at) = (None, 0);
            for (i, line) in lines.iter().enumerate() {
                let next = lines.get(i + 1).map_or("", |l| l.trim());
                let next = next.strip_prefix("pub(crate) ").or(next.strip_prefix("pub ")).unwrap_or(next);
                let cfg = line.trim_start().starts_with("#[cfg(") && line.contains("test");
                if let (true, Some(module)) = (cfg, next.strip_prefix("mod ")) {
                    match module.strip_suffix(';') {
                        Some(name) => own_files.push(format!("{}.rs", name.trim())),
                        None => start = start.or(Some(at)),
                    }
                }
                at += line.len();
            }
            starts.push(start);
        }
        files
            .into_iter()
            .zip(starts)
            .filter_map(|((name, text), start)| {
                let own = own_files.iter().any(|f| name.rsplit('/').next() == Some(f.as_str()));
                match (own, start) {
                    (true, _) => Some((name, text)),
                    (false, Some(from)) => Some((name, text[from..].to_string())),
                    (false, None) => None,
                }
            })
            .collect()
    }

    #[test]
    fn no_test_takes_the_real_bundle_lock() {
        // Every way to reach the real lock NAMES one of these two: a call, a path such as
        // `&crate::self_update::BUNDLE_WORK` or `&super::BUNDLE_WORK`, a `use`, or a function
        // pointer such as `&crate::self_update::claim_bundle_work` - which is how production hands
        // the closer the lock. So the rule is on the name, outside comments and literals, and a test
        // that needs one for a needle spells it inside a string. It holds for the test code of every
        // file in this crate, found on disk rather than listed.
        let code = test_code(&crate_src());
        assert!(
            code.iter().any(|(file, _)| file == "self_update.rs"),
            "the scan found no test code even in self_update.rs, so it proves nothing: {:?}",
            code.iter().map(|(file, _)| file).collect::<Vec<_>>()
        );
        for (file, src) in &code {
            for line in src.lines().map(str::trim).filter(|l| !l.starts_with("//")) {
                let bare = outside_literals(line);
                for name in ["BUNDLE_WORK", "claim_bundle_work"] {
                    assert!(
                        !bare.contains(name),
                        "a test in {file} reaches the real BUNDLE_WORK: `{line}`. The tests in this \
                         binary run in parallel; take a lock of the test's own and pass it in."
                    );
                }
            }
        }
    }

    #[test]
    fn the_lock_guard_reads_every_test_module_and_every_file_added_to_the_crate() {
        // A crate of its own, grown a file at a time: what the guard reads must grow with it.
        let s = Scratch::new("guard-walk");
        let found = |dir: &std::path::Path| -> Vec<String> {
            test_code(dir).into_iter().map(|(name, _)| name).collect()
        };
        std::fs::write(s.0.join("a.rs"), "fn a() {}\n#[cfg(test)]\nmod tests {\n}\n").expect("a.rs");
        std::fs::write(s.0.join("b.rs"), "fn b() {}\n").expect("b.rs");
        assert_eq!(found(&s.0), ["a.rs"]);
        assert_eq!(
            test_code(&s.0)[0].1,
            "#[cfg(test)]\nmod tests {\n}\n",
            "a file's test code starts at its test module's attribute"
        );
        std::fs::create_dir_all(s.0.join("deeper")).expect("a folder");
        std::fs::write(
            s.0.join("deeper").join("c.rs"),
            "#[cfg(all(test, target_os = \"macos\"))]\npub(crate) mod mac {\n}\n",
        )
        .expect("c.rs");
        std::fs::write(s.0.join("d.rs"), "#[cfg(test)]\nmod e;\n").expect("d.rs");
        std::fs::write(s.0.join("e.rs"), "fn called_only_by_tests() {}\n").expect("e.rs");
        assert_eq!(
            found(&s.0),
            ["a.rs", "deeper/c.rs", "e.rs"],
            "a file added to the crate - in a folder, with its tests under any cfg that names test, \
             or in a file of their own - must be read without anybody listing it"
        );

        // And this crate: every module main.rs declares is a file the walk reads.
        let src = crate_src();
        let walked: Vec<String> = rust_files(&src).into_iter().map(|(name, _)| name).collect();
        let main = std::fs::read_to_string(src.join("main.rs")).expect("main.rs can be read");
        let declared: Vec<&str> =
            main.lines().filter_map(|l| l.strip_prefix("mod ")?.strip_suffix(';')).collect();
        assert!(declared.contains(&"self_update"), "main.rs's modules were not found: {declared:?}");
        for m in declared {
            assert!(
                walked.contains(&format!("{m}.rs")),
                "main.rs declares `mod {m};` and the walk never reads {m}.rs: {walked:?}"
            );
        }
    }

    // ---- the swap lock, across launcher processes ----------------------------------------------

    #[test]
    fn the_swap_lock_has_one_holder_at_a_time_and_is_let_go_with_its_holder() {
        let s = Scratch::new("swap-lock");
        let path = s.0.join("state").join(SWAP_LOCK_FILE);
        let first = try_swap_lock(&path)
            .expect("the swap lock opens, and its folder is made")
            .expect("nobody holds it yet");
        assert!(
            try_swap_lock(&path).expect("the swap lock opens").is_none(),
            "a second holder took the swap lock while the first still held it"
        );
        drop(first);
        // Free again once it is let go of - and once no process this one was starting at that
        // instant still carries a copy of its file, which a starting process does until it runs its
        // own program. This test binary starts processes all the time, so that instant is waited out.
        let freed = (0..200).any(|_| {
            try_swap_lock(&path).expect("the swap lock opens").is_some() || {
                std::thread::sleep(std::time::Duration::from_millis(10));
                false
            }
        });
        assert!(freed, "a swap lock that was let go of must be free again");
    }

    /// What turns the next test into a launcher process of its own.
    const HOLD_SWAP_LOCK: &str = "--hold-swap-lock";

    /// THE OTHER LAUNCHER PROCESS of `a_second_launcher_process_is_shut_out_until_the_first_is_gone`,
    /// and nothing on its own: started by that test with `HOLD_SWAP_LOCK` and a path on its command
    /// line, it takes the swap lock there and holds it until its input closes or it is killed.
    #[test]
    fn a_launcher_process_that_holds_the_swap_lock() {
        use std::io::{Read, Write};
        let args: Vec<String> = std::env::args().collect();
        let Some(at) = args.iter().position(|a| a == HOLD_SWAP_LOCK) else {
            return;
        };
        let path = std::path::PathBuf::from(&args[at + 1]);
        let held = try_swap_lock(&path).expect("the swap lock opens").expect("nobody holds it yet");
        println!("holding");
        let _ = std::io::stdout().flush();
        let _ = std::io::stdin().read_to_end(&mut Vec::new());
        drop(held);
    }

    /// TWO LAUNCHER PROCESSES, FOR REAL: this test binary started again, running only the test
    /// above, holds the swap lock while this one asks for it - and is then killed, the way a crash
    /// or a force quit ends a launcher.
    #[test]
    fn a_second_launcher_process_is_shut_out_until_the_first_is_gone() {
        use std::io::BufRead;
        let s = Scratch::new("swap-lock-processes");
        let path = s.0.join(SWAP_LOCK_FILE);
        let mut first = std::process::Command::new(std::env::current_exe().expect("this test binary"))
            .args(["--exact", "--nocapture", "--"])
            .arg("self_update::tests::a_launcher_process_that_holds_the_swap_lock")
            .arg(HOLD_SWAP_LOCK)
            .arg(&path)
            .stdin(std::process::Stdio::piped())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null())
            .spawn()
            .expect("a second launcher process can be started");
        let mut said = std::io::BufReader::new(first.stdout.take().expect("its output")).lines();
        assert!(
            said.by_ref().map_while(Result::ok).any(|l| l == "holding"),
            "the other launcher process never took the swap lock"
        );
        assert!(
            try_swap_lock(&path).expect("the swap lock opens").is_none(),
            "another launcher process holds the swap lock, and this one took it too: their exchanges \
             and exchanges back could interleave"
        );
        first.kill().expect("the other process can be ended");
        let _ = first.wait();
        assert!(
            try_swap_lock(&path).expect("the swap lock opens").is_some(),
            "a launcher that died holding the swap lock kept it, so every later launcher would leave \
             its update to a process that is gone"
        );
        drop(said);
    }

    // ---- the home screen ------------------------------------------------------------------------

    const MAIN_WEB: &str = include_str!("../../../../design/launcher/screens/MainWeb.html");
    const MAIN_UPDATE: &str = include_str!("../../../../design/launcher/screens/MainUpdate.html");

    /// HTML comments and whole-line `//` comments out: the prose in these pages names MainUpdate.html
    /// and every say() owner while explaining them.
    fn code_only(html: &str) -> String {
        let mut out = String::new();
        let mut rest = html;
        while let Some(at) = rest.find("<!--") {
            out.push_str(&rest[..at]);
            match rest[at..].find("-->") {
                Some(end) => rest = &rest[at + end + 3..],
                None => {
                    rest = "";
                    break;
                }
            }
        }
        out.push_str(rest);
        out.lines()
            .filter(|l| !l.trim_start().starts_with("//"))
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// From `head` to the brace that closes the first `{` at or after it.
    fn js_block<'a>(code: &'a str, head: &str) -> &'a str {
        let start = code.find(head).unwrap_or_else(|| panic!("`{head}` is gone from the page"));
        let open = start + code[start..].find('{').expect("a block");
        let mut depth = 0usize;
        for (i, ch) in code[open..].char_indices() {
            match ch {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        return &code[start..open + i + 1];
                    }
                }
                _ => {}
            }
        }
        panic!("`{head}` is never closed");
    }

    /// Every top-level `(function () {` block in the page, in source order. Nested functions are
    /// inside their block and are skipped with it.
    fn iifes(code: &str) -> Vec<&str> {
        let head = "(function () {";
        let mut v = Vec::new();
        let mut from = 0;
        while let Some(i) = code[from..].find(head) {
            let at = from + i;
            let b = js_block(&code[at..], head);
            v.push(b);
            from = at + b.len();
        }
        v
    }

    /// Is `word` in `hay` as a BARE identifier - not part of a longer name, not a `.property`?
    fn mentions(hay: &str, word: &str) -> bool {
        let is_id = |c: char| c.is_ascii_alphanumeric() || c == '_' || c == '$';
        let mut from = 0;
        while let Some(i) = hay[from..].find(word) {
            let at = from + i;
            let before = hay[..at].chars().next_back();
            let after = hay[at + word.len()..].chars().next();
            if !before.map_or(false, is_id) && !after.map_or(false, is_id) && before != Some('.') {
                return true;
            }
            from = at + word.len();
        }
        false
    }

    /// NO ELEMENT HANDLE IS REACHED FOR FROM ANOTHER FUNCTION SCOPE.
    ///
    /// THE DEFECT THIS HOLDS, measured 18/09/2026. `launcherEl` was captured with `valueAfter` in
    /// one IIFE and read in another, where it does not exist. That threw `ReferenceError` as the
    /// first statement of the routing handler, the chain's `.catch(function () {})` swallowed it,
    /// and so the Launcher row stayed a dash AND everything after the throw - the setup-page button
    /// and the automatic engine install - silently never ran, from 9a4f2c2f5 (13/09/2026) on.
    ///
    /// A `.catch(function () {})` makes this class invisible at runtime by construction, which is
    /// why it is checked here, in the source, where it cannot be swallowed. It checks the CLASS -
    /// every handle, every other block - not just the one name that happened to break.
    #[test]
    fn no_element_handle_is_reached_for_from_another_function_scope() {
        let code = code_only(MAIN_WEB);
        let blocks = iifes(&code);
        assert!(blocks.len() >= 3, "expected the page's several IIFEs, found {}", blocks.len());

        let declares = |b: &str, name: &str| b.contains(&format!("var {name} ="));
        let mut handles: Vec<(usize, String)> = Vec::new();
        for (bi, b) in blocks.iter().enumerate() {
            let mut from = 0;
            while let Some(i) = b[from..].find("var ") {
                let at = from + i + 4;
                let name: String =
                    b[at..].chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '_').collect();
                let rest = &b[at + name.len()..];
                if !name.is_empty()
                    && (rest.starts_with(" = valueAfter(") || rest.starts_with(" = textDiv("))
                {
                    handles.push((bi, name));
                }
                from = at;
            }
        }
        assert!(
            handles.iter().any(|(_, n)| n == "launcherEl"),
            "the Launcher row's handle is no longer captured by valueAfter - re-point this test"
        );

        for (home, name) in &handles {
            for (bi, b) in blocks.iter().enumerate() {
                if bi == *home || declares(b, name) {
                    continue;
                }
                assert!(
                    !mentions(b, name),
                    "`{name}` is captured in function block #{home} but read in block #{bi}, where \
                     it does not exist - that throws ReferenceError, and a .catch(function () {{}}) \
                     will hide it. Expose a painter on window from block #{home}, as gxSay and \
                     gxPaintLauncher do."
                );
            }
        }

        // And the routing block paints the row the way that works.
        let routing = js_block(&code, "Promise.all([invoke('about_info'), invoke('game_status')]).then(");
        assert!(
            routing.contains("window.gxPaintLauncher(r[0])"),
            "the routing handler no longer hands about_info to the Launcher row's painter: {routing}"
        );
    }

    /// "PLAYERS CAN JOIN" IS PAINTED FROM A FACT OR HIDDEN - NEVER THE OLD SAMPLE (18/09/2026).
    ///
    /// It read "any platform" on every install, as fixed markup, whatever the install could actually
    /// join. Two ways to bring that back, and this holds both: put the claim back in the markup, or
    /// stop painting the row from `joins` (the markup would then be what shows).
    #[test]
    fn the_players_can_join_row_is_painted_from_joins_not_fixed_text() {
        let label = ">Players can join</span>";
        let at = MAIN_WEB.find(label).expect("the Players can join row is gone from MainWeb.html");
        let after = &MAIN_WEB[at + label.len()..];
        let open = after.find("<span").expect("no value span after the label");
        let gt = open + after[open..].find('>').expect("unterminated span");
        let end = gt + after[gt..].find("</span>").expect("unclosed value span");
        let value = after[gt + 1..end].trim();
        assert_eq!(
            value, "—",
            "the markup's value for 'Players can join' is {value:?}: it must be a placeholder, never a \
             claim - the row is painted from release_state's `joins`"
        );
        assert!(!MAIN_WEB.to_lowercase().contains(">any platform<"), "the fixed 'any platform' sample is back");

        let code = code_only(MAIN_WEB);
        assert!(code.contains("var joinsEl = valueAfter('Players can join');"), "the row's handle is gone");
        let paint = js_block(&code, "function paintRelease(s, rank) {");
        assert!(
            paint.contains("joinsEl.textContent = s.joins") && paint.contains("setRowHidden(joinsEl, !s.joins);"),
            "paintRelease no longer paints the row from `joins`, or no longer hides it when empty: {paint}"
        );
    }

    /// UNHIDING A ROW PUTS ITS OWN LAYOUT BACK (18/09/2026). Every THIS INSTALL row is laid out by an
    /// inline `display: flex`, so `style.display = ''` does not unhide it - it deletes the flex and
    /// the value drops against its label. That shipped in cb8db1f33 and only a screenshot showed it.
    #[test]
    fn unhiding_a_row_restores_its_own_display_not_an_empty_one() {
        let code = code_only(MAIN_WEB);
        let f = js_block(&code, "function setRowHidden(valueEl, hidden) {");
        assert!(
            !f.contains("? 'none' : ''") && !f.contains("? \"none\" : \"\""),
            "setRowHidden unhides by writing an EMPTY display, which erases the row's inline flex: {f}"
        );
        assert!(
            f.contains("data-gx-display"),
            "setRowHidden no longer remembers the row's own display to put it back: {f}"
        );
    }

    /// EVERY PLATFORM HANDS A CONFIRMED UPDATE OVER, macOS INCLUDED (28/09/2026, wh1ter0se: "macOS
    /// launcher should behave just the same as windows going yellow for an update button"). macOS
    /// was kept on MainWeb because "handing over would put PLAY behind a page with no way back"; it now
    /// updates on UPDATE only, and MainUpdate has that way back once UPDATE cannot deliver. So the
    /// gate names no platform, and the hand-over still stands down on a test profile and once the
    /// player took the way back (`updateLater`). scripts/test/launcher-macos-update-test.js drives it.
    #[test]
    fn the_home_screen_hands_every_platform_over_to_the_update_page() {
        let code = code_only(MAIN_WEB);
        let paint = js_block(&code, "function paintRelease(s, rank) {");
        let page = format!("{}.html", "MainUpdate");
        let routes: Vec<&str> = paint.lines().filter(|l| l.contains(&page)).collect();
        assert_eq!(routes.len(), 1, "paintRelease must hand over in exactly one place: {routes:?}");
        assert!(
            routes[0].contains("if (!s.installs_off && !updateLater() && releaseDrivesNavigation(s.verdict, s.platform, rank))"),
            "the hand-over must stand down on a test profile and after the way back: {}",
            routes[0].trim()
        );
        assert!(!code.replacen(paint, "", 1).contains(&page), "a second route to {page}, outside paintRelease");
        let decide = js_block(&code, "function releaseDrivesNavigation(verdict, platform, rank) {");
        assert!(
            !decide.contains("macos") && decide.contains("verdict === 'update'"),
            "the hand-over singles a platform out again: {decide}"
        );
        let later = js_block(&code, "function updateLater() {");
        assert!(
            later.contains("sessionStorage.getItem('gx-update-later')") && later.contains("catch (e) { return true; }"),
            "the way back must hold for this launcher run, and a storage throw must read as 'stay': {later}"
        );
        let update = code_only(MAIN_UPDATE);
        assert!(
            update.contains("window.sessionStorage.setItem('gx-update-later', '1')")
                && update.contains("laterBtn.style.display = mode === 'check' ? '' : 'none';"),
            "MainUpdate's way back is gone, or shows before UPDATE has failed"
        );
    }

    /// DEFECT, PINNED (Codex, 4th round against ab3b1e10e). `release_state`'s disk-only "update" is
    /// `decide`'s own UNCONFIRMED guess - for a record that is stale in a way the real updater does
    /// not agree with, `GeneralsXUpdate.exe --check` can answer an authoritative EMPTY plan
    /// (`an_empty_updater_plan_downgrades_a_derived_update`'s exact shape, cmd_release.rs). A record
    /// like that does not rewrite itself, so the guess is not a one-off race that sometimes loses -
    /// it is a STABLE STATE: every visit repeats it. Two rounds tried fixing this by giving the
    /// Rust side memory of the real answer (a cache, then one keyed to the install record); both
    /// were reverted because they put state back on a path the owner wants to stay empty. The fix
    /// that adds no state at all: only a CONFIRMED release answer (rank 2, from `release_check`)
    /// may drive navigation. Fails on revert: dropping `rank >= 2` (or the parameter itself) makes
    /// this read `true` for the disk-only rank.
    #[test]
    fn an_unconfirmed_disk_verdict_never_drives_navigation() {
        let code = code_only(MAIN_WEB);
        let decide = js_block(&code, "function releaseDrivesNavigation(verdict, platform, rank) {");
        assert!(
            decide.contains("rank >= 2"),
            "the disk-only rank (1) can drive navigation again - this is the permanent redirect \
             loop between MainWeb and MainUpdate for a stale record the real updater has already \
             downgraded: {decide}"
        );
        // And the rank that reaches it is release_check's, not release_state's: onRelease(1) is the
        // disk answer, onRelease(2) the network one, and paintRelease must receive whichever rank
        // produced it rather than a fixed or dropped argument.
        let on_release = js_block(&code, "function onRelease(rank) {");
        assert!(
            on_release.contains("paintRelease(s, rank);"),
            "onRelease no longer passes its own rank through to paintRelease, so the gate above has \
             nothing to gate on: {on_release}"
        );
        assert!(
            code.contains("invoke('release_state').then(onRelease(1))"),
            "the disk-only read is no longer ranked 1 (unconfirmed)"
        );
        assert!(
            code.contains("invoke('release_check').then(onRelease(2))"),
            "the network check is no longer ranked 2 (confirmed)"
        );
    }

    #[test]
    fn the_update_verdict_is_painted_in_place_and_the_background_update_keeps_its_label() {
        let code = code_only(MAIN_WEB);
        let labels = js_block(&code, "var VERDICT_LABEL = {");
        assert!(
            labels.contains("update: 'Update available',"),
            "with no label the card prints the raw token 'update': {labels}"
        );
        // RE-PINNED 18/09/2026, SAME INTENT. statusText now takes the whole release state rather
        // than the bare verdict, because a development copy's headline reads installed_from and
        // installed_unpublished as well. The precedence rule this test exists for is unchanged and
        // is still stated in exactly one place - that is what the two asserts below hold.
        let paint = js_block(&code, "function paintRelease(s, rank) {");
        assert!(
            paint.contains("lastState = s;")
                && paint.contains("statusLabel.textContent = statusText(s);"),
            "a verdict that lands after the update started must not paint over the label that says \
             so: {paint}"
        );
        let text = js_block(&code, "function statusText(s) {");
        assert!(
            text.contains("return SELF_LABEL[selfPhase] ||"),
            "while the background update runs, its own label wins over the verdict's: {text}"
        );
        // And the self-update repaint rebuilds from the WHOLE state, or it would undo the words
        // paintRelease chose for a development copy.
        let selfp = js_block(&code, "function paintSelfUpdate(u) {");
        assert!(
            selfp.contains("statusLabel.textContent = statusText(lastState);"),
            "the background update's repaint must rebuild from the last state: {selfp}"
        );
    }

    #[test]
    fn the_home_screen_follows_the_background_update_under_its_own_owner() {
        let code = code_only(MAIN_WEB);
        assert!(
            code.contains("invoke('release_self_update_status').then(paintSelfUpdate)"),
            "a page that loads after the update reported never hears of it"
        );
        assert!(
            code.contains(".listen('gx-self-update'"),
            "no listener for the background update's event, `gx-self-update`"
        );

        let say_at = code.find("function say(text, tone, owner) {").expect("MainWeb's say() is gone");
        let def = code
            .find("window.gxSayUpdate = function (text, tone) {")
            .expect("MainWeb has no writer for the background update");
        assert!(def > say_at, "the writer must live beside say(), where the owner rule is");
        let writer = js_block(&code, "window.gxSayUpdate = function (text, tone) {");
        assert!(
            writer.contains("say(text, tone, 'selfupdate');"),
            "the background update must write under its OWN owner - not game, release, apply or \
             engine: {writer}"
        );
        assert!(
            writer.contains(
                "if (text && msgOwner !== null && msgOwner !== 'release' && msgOwner !== 'selfupdate') { return; }"
            ),
            "it must yield every line it does not own except the release tier's - the engine note is \
             taken off disk when it is shown, and would be lost under this one: {writer}"
        );

        let paint = js_block(&code, "function paintSelfUpdate(u) {");
        assert!(
            paint.contains("var note = window.gxSayUpdate"),
            "the background update's words must go through its own writer: {paint}"
        );
        for never in ["location", "game_play", "alert(", "confirm(", ".focus("] {
            assert!(
                !paint.contains(never),
                "the background update must never `{never}` - it reports, it does not take over: \
                 {paint}"
            );
        }
    }

    /// A PLAY PRESS WAITING OUT AN ENGINE UPDATE KEEPS ITS SENTENCE. While an engine update is
    /// applied the line beside PLAY says why the press is waiting (owner 'apply'), and on macOS the
    /// release tier writes its own dim sentence on every load: say() used to let that, or any other
    /// owner's softer line, paint over the wait. One owner is exempt, the engine note: it is already
    /// off disk when it is painted, so refused it would be lost unseen, and its own gate keeps it off
    /// a press in flight except for the moment its read is in flight.
    #[test]
    fn a_play_press_waiting_out_an_engine_update_keeps_its_sentence() {
        let code = code_only(MAIN_WEB);
        let say = js_block(&code, "function say(text, tone, owner) {");
        let rule = say
            .lines()
            .map(str::trim)
            .find(|l| l.starts_with("if (msgOwner === 'apply'"))
            .unwrap_or_else(|| {
                panic!(
                    "say() lets another owner's softer line paint over the sentence a PLAY press is \
                     waiting behind while an engine update is applied: {say}"
                )
            });
        assert!(
            rule.contains("owner !== 'engine'"),
            "the engine note is already off disk when it is painted; held off the wait it is lost \
             unseen, so it alone is exempt: {rule}"
        );
        for other in ["'release'", "'selfupdate'", "'game'"] {
            assert!(
                !rule.contains(&format!("owner !== {other}")),
                "only the engine note is exempt - {other} says its sentence again on the next load: \
                 {rule}"
            );
        }
        assert_eq!(
            rule,
            "if (msgOwner === 'apply' && owner !== 'apply' && owner !== 'engine' && tone !== 'bad') { return; }",
            "a 'bad' line still displaces the wait, and the wait's own owner still corrects itself"
        );
        let at = say.find(rule).expect("found above");
        let taken = say.find("msgOwner = owner;").expect("say() no longer records who holds the line");
        assert!(at < taken, "the rule must stand before the line is taken, or it guards nothing");
        assert!(
            MAIN_WEB.contains("A line owned by 'apply' is a live wait"),
            "say()'s header must state this rule beside the others"
        );
    }

    #[test]
    fn the_update_page_no_longer_says_the_unix_apply_does_not_exist() {
        for stale in ["it does not exist", "apply does not exist"] {
            assert!(
                !MAIN_UPDATE.contains(stale),
                "MainUpdate still says `{stale}`; release_apply has had a macOS and Linux arm since \
                 25/08/2026"
            );
        }
        assert!(
            MAIN_UPDATE.contains("on every platform - on\n// macOS too since 28/09/2026"),
            "MainUpdate's header must say which platforms hand over to it"
        );
    }

    // ---- when it runs, what it leaves behind, how a new launcher answers, and the fixture ----

    /// NOTHING STARTS IT BUT A PRESS (28/09/2026). It used to start from main.rs's `.setup` on every
    /// macOS launch, unasked; the owner asked for macOS to update like Windows, on UPDATE. So
    /// `run_pressed` has ONE production caller - release_apply's macOS arm, after its test-profile
    /// refusal - and nothing in `.setup` starts an update. Red arm: put a spawn back in `.setup`, or
    /// call it from anywhere else.
    #[test]
    fn nothing_starts_the_launchers_own_update_but_a_press() {
        let prod = |src: &'static str| &src[..src.find("#[cfg(test)]").unwrap_or(src.len())];
        let main = prod(include_str!("main.rs"));
        let setup = &main[main.find(".setup(move |app| {").expect("the setup hook")..];
        let setup = &setup[..setup.find(".run(context)").expect("the run call")];
        let code: Vec<&str> = setup.lines().map(str::trim).filter(|l| !l.starts_with("//")).collect();
        assert!(
            !code.iter().any(|l| l.contains("self_update::") && !l.contains("identify")),
            "`.setup` reaches the launcher's own update again, unasked: {code:#?}"
        );
        let release = prod(include_str!("cmd_release.rs"));
        assert_eq!(release.matches("crate::self_update::run_pressed(").count(), 1, "one press, one caller");
        let apply = &release[release.find("pub fn release_apply(").expect("release_apply")..];
        let refuse = apply.find("gxio::release::refuse_install_on_test_profile()?;").expect("the test-profile refusal");
        let mac = apply.find("#[cfg(target_os = \"macos\")]").expect("the macOS arm");
        let call = apply.find("crate::self_update::run_pressed(").expect("the press");
        assert!(refuse < mac && mac < call, "the press runs it, in the macOS arm, after the test-profile refusal");
        for (file, src) in [("main.rs", main), ("cmd_setup.rs", prod(include_str!("cmd_setup.rs"))),
                            ("cmd_game.rs", prod(include_str!("cmd_game.rs")))] {
            assert!(!src.contains("run_pressed("), "{file} starts the launcher's own update");
        }
    }

    #[test]
    fn the_log_line_names_the_time_the_phase_and_this_launcher() {
        let s = SelfUpdateOutcome::Installed { name: NEWER.into(), recorded: true }.status();
        assert_eq!(
            log_line(&s, "aaaaaaaaa", 1_789_129_912),
            format!(
                "1789129912 installed launcher=aaaaaaaaa {NEWER} It takes over the next time you \
                 open the launcher.\n"
            )
        );
    }

    #[test]
    fn the_commit_time_is_read_as_a_positive_number_or_not_at_all() {
        assert_eq!(
            parse_epoch(Some("1789129912")),
            Some(1_789_129_912),
            "a packaged build's commit time must be read, or every packaged launcher is a \
             development build that never updates itself"
        );
        assert_eq!(parse_epoch(Some(" 1789129912\n")), Some(1_789_129_912));
        for none in [None, Some(""), Some("0"), Some("soon"), Some("-1"), Some("1789129912.5")] {
            assert_eq!(parse_epoch(none), None, "{none:?} is not a commit time");
        }
        let src = include_str!("self_update.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod tests {").expect("the test module is gone")];
        let own = &prod[prod.find("pub(crate) fn own_build()").expect("own_build is gone")..];
        let own = &own[..own.find("\n}\n").expect("own_build closes in column 0")];
        assert!(
            own.contains("commit_epoch: parse_epoch(option_env!(\"GX_COMMIT_EPOCH\")),"),
            "own_build must read the commit time through parse_epoch, the reading this test holds: \
             {own}"
        );
    }

    #[test]
    fn the_backoff_record_is_one_line_naming_the_time_and_the_artifact() {
        let line = backoff_line(1_789_129_912, NEWER);
        assert_eq!(line, format!("1789129912 {NEWER}\n"));
        assert_eq!(
            parse_backoff(&line),
            Some((1_789_129_912, NEWER.to_string())),
            "what the backoff writes must read back, or a failure is downloaded again every launch"
        );
        for junk in ["", "soon GeneralsX.tar.gz", "1789129912", "1789129912 a b"] {
            assert_eq!(parse_backoff(junk), None, "{junk:?} must hold nothing back");
        }
    }

    #[test]
    fn what_a_launcher_says_about_itself_is_what_the_swap_accepts() {
        let said = identify_line(OwnBuild { commit9: "bbbbbbbbb", commit_epoch: Some(1_789_129_912) });
        assert_eq!(
            probe_verdict(&ProbeRun::Exited { code: Some(0), stdout: format!("{said}\n") }, "bbbbbbbbb"),
            Ok(()),
            "the line a new launcher prints must be the line the swap accepts: {said}"
        );
        let dev = identify_line(OwnBuild { commit9: "bbbbbbbbb", commit_epoch: None });
        assert!(
            probe_verdict(&ProbeRun::Exited { code: Some(0), stdout: dev }, "bbbbbbbbb").is_err(),
            "a development build names no commit time, and must not be kept"
        );
        assert_eq!(IDENTIFY_FLAG, "--gx-identify");
    }

    /// A stand-in launcher: a shell script at `dir/name`, made executable.
    #[cfg(target_os = "macos")]
    fn stand_in(dir: &std::path::Path, name: &str, body: &str) -> std::path::PathBuf {
        use std::os::unix::fs::PermissionsExt;
        let p = dir.join(name);
        std::fs::write(&p, format!("#!/bin/sh\n{body}\n")).expect("the stand-in can be written");
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755))
            .expect("the stand-in can be made executable");
        p
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn the_new_launcher_is_started_with_the_identify_flag_and_heard_out() {
        let s = Scratch::new("identify-run");
        let echoes = stand_in(&s.0, "echoes", "echo \"$1\"");
        assert_eq!(
            run_identify(&echoes, PROBE_TIMEOUT),
            ProbeRun::Exited { code: Some(0), stdout: "--gx-identify\n".to_string() },
            "the new launcher must be started with the identify flag, and its answer read"
        );
        let fails = stand_in(&s.0, "fails", "exit 3");
        assert_eq!(
            run_identify(&fails, PROBE_TIMEOUT),
            ProbeRun::Exited { code: Some(3), stdout: String::new() }
        );
        assert!(
            matches!(run_identify(&s.0.join("absent"), PROBE_TIMEOUT), ProbeRun::NotStarted(_)),
            "an executable that cannot be started is not an answer"
        );
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn a_new_launcher_that_hangs_is_killed_when_the_time_runs_out() {
        let s = Scratch::new("identify-hang");
        let pid_file = s.0.join("pid");
        let hangs = stand_in(
            &s.0,
            "hangs",
            &format!("echo $$ > '{}'\nexec /bin/sleep 30", pid_file.display()),
        );
        // Two seconds is plenty for the stand-in to start and say its pid, unless the machine is busy
        // starting other processes; then a longer limit is tried, rather than nothing checked.
        let pid = [2, 5, 10]
            .into_iter()
            .find_map(|secs| {
                let run = run_identify(&hangs, std::time::Duration::from_secs(secs));
                assert_eq!(run, ProbeRun::TimedOut);
                std::fs::read_to_string(&pid_file).ok()
            })
            .expect("the stand-in never started and wrote its pid, even given ten seconds");
        let alive = std::process::Command::new("/bin/kill")
            .args(["-0", pid.trim()])
            .stderr(std::process::Stdio::null())
            .status()
            .expect("kill can be run")
            .success();
        assert!(
            !alive,
            "a launcher that did not answer in time must be killed, not left running: pid {}",
            pid.trim()
        );
    }

    #[test]
    fn a_fixture_is_read_whole_or_refused() {
        let good = "own 000000001 1700000000\ndocument 1789129912\n\
                    row GeneralsXLauncher-macos-7563609dc.tar.gz AAAAAAAAAAAAAAAAAAAAAA 4096\n";
        let fx = parse_fixture(good).expect("a complete fixture");
        assert_eq!((fx.own_commit9.as_str(), fx.own_epoch), ("000000001", 1_700_000_000));
        assert_eq!(fx.row_name, "GeneralsXLauncher-macos-7563609dc.tar.gz");
        assert_eq!(fx.manifest.ordinal, 1_789_129_912);
        assert_eq!(
            fx.manifest.artifacts,
            [gxcore::Artifact {
                platform: "macos".into(),
                name: fx.row_name.clone(),
                hash: "AAAAAAAAAAAAAAAAAAAAAA".into(),
                size: 4096,
            }]
        );
        let own = OwnBuild { commit9: &fx.own_commit9, commit_epoch: Some(fx.own_epoch) };
        assert!(
            matches!(plan_self_update(&fx.manifest, "macos", own), SelfUpdatePlan::Install(_)),
            "the rule must see a fixture's row as a newer launcher, or the fixture exercises nothing"
        );
        for broken in [
            good.replace("document 1789129912\n", ""),
            good.replace("own 000000001 1700000000", "own 000000001 soon"),
            good.replace(" 4096", ""),
            format!("{good}relay somewhere\n"),
        ] {
            assert!(parse_fixture(&broken).is_err(), "a fixture that is not whole was accepted:\n{broken}");
        }
    }

    /// THE FIXTURE REACHES THE DECISION (Codex rel0949-r2). Only a press runs the launcher's own update
    /// since 28/09/2026, and a press is reached through the verdict - so in the offline account the
    /// fixture exists for, the verdict and the press must both judge the fixture's document against the
    /// fixture's identity, or the fixture is never read. Red arm: drop either read, or answer something
    /// other than `None` outside a fixture build (a packaged launcher must never judge by a fixture).
    #[test]
    fn a_fixture_build_decides_its_update_from_the_fixture() {
        // Outside a fixture build - every packaged one - it never answers. A fixture build answers only
        // with GX_SELFUPDATE_FIXTURE set, which is that build's point (Codex q7-macupd-r6).
        if !cfg!(gx_selfupdate_fixture) {
            assert!(fixture_world().is_none(), "a build without the fixture cfg must never answer a fixture");
        }
        let src = include_str!("cmd_release.rs");
        let prod = &src[..src.find("#[cfg(test)]").expect("the tests")];
        assert_eq!(
            prod.matches("crate::self_update::fixture_world();").count(),
            2,
            "the verdict and the press both ask the fixture, and nothing else does"
        );
        let body = |head: &str| {
            let at = prod.find(head).unwrap_or_else(|| panic!("`{head}` is gone"));
            &prod[at..at + prod[at..].find("\n}\n").expect("closes in column 0")]
        };
        for (what, f) in [("the verdict", body("fn build_release_state(")), ("the press", body("pub fn release_apply("))] {
            assert!(
                f.contains("Some((m, _)) => (Some(m.clone()), \"fixture\"),")
                    && f.contains("fixture.map_or_else(crate::self_update::own_build, |(_, own)| own)"),
                "{what} does not judge the fixture's document against the fixture's identity: {f}"
            );
        }
        assert!(body("pub fn release_apply(").contains("launcher_outstanding(&m, platform, own, Some(&installed), false)"));
    }

    #[test]
    fn only_a_fixture_build_can_read_the_fixture_folder() {
        let src = include_str!("self_update.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod tests {").expect("the test module is gone")];
        let var = format!("\"{}\"", "GX_SELFUPDATE_FIXTURE");
        let reads: Vec<usize> = prod.match_indices(var.as_str()).map(|(at, _)| at).collect();
        assert_eq!(reads.len(), 1, "the fixture folder must be read in exactly one place: {reads:?}");
        let line_start = prod[..reads[0]].rfind('\n').map_or(0, |n| n + 1);
        let above: Vec<&str> = prod[..line_start]
            .lines()
            .rev()
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with("//"))
            .take(2)
            .collect();
        assert_eq!(
            above,
            ["fn fixture_dir() -> Option<std::path::PathBuf> {", "#[cfg(gx_selfupdate_fixture)]"],
            "the fixture folder is read outside a `#[cfg(gx_selfupdate_fixture)]` function, so a \
             packaged launcher could honour it"
        );
    }

    /// The body of production's `fn <name>`, comment lines out, up to the brace that closes it in
    /// column 0.
    fn production_body(sig: &str) -> Vec<String> {
        let src = include_str!("self_update.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod tests {").expect("the test module is gone")];
        let at = prod.find(sig).unwrap_or_else(|| panic!("`{sig}` is gone from self_update.rs"));
        let end = prod[at..].find("\n}\n").map_or(prod.len(), |e| at + e);
        prod[at..end]
            .lines()
            .skip(1)
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with("//"))
            .map(str::to_string)
            .collect()
    }

    /// THE SWAP'S OWN SIDE OF THE INTERLOCK, AND ITS ONE WAY IN. Production's seams are the only
    /// place the real locks and the real exchange reach the pipeline, and no test may take the one or
    /// run the other there - so this reads them. The hold must be the REAL bundle lock (any other
    /// lets the swap land inside an engine install); the apply must be `apply_launcher` under the
    /// swap lock every launcher process shares, with the exchange-only swap, and heard out by the
    /// new launcher's own executable (the pressed update's apply falls back to two renames).
    #[test]
    fn the_production_swap_holds_the_real_locks_and_never_falls_back() {
        let body = production_body("fn with_production_seams<R>(");
        let real = ["let hold = || wait_for_bundles(&", "BUNDLE_WORK", ", BUNDLE_WAIT, &|| std::thread::sleep(BUNDLE_POLL));"]
            .concat();
        assert!(
            body.iter().any(|l| *l == real),
            "the swap does not wait on the REAL bundle lock, so it can land in the middle of an \
             engine install: {body:#?}"
        );
        assert!(body.iter().any(|l| l == "hold_bundles: &hold,"), "the seams are not handed that hold: {body:#?}");
        let all = body.join("\n");
        assert!(
            all.contains(
                "let probe = |b: &std::path::Path| run_identify(&bundle_executable(b), PROBE_TIMEOUT);\n\
                 apply_launcher(\narchive,\nbundle,\ncommit9,\n&take_swap_lock,\n\
                 &gxio::update::exchange_bundles,\n&probe,\n)"
            ),
            "the swap must be apply_launcher, under the swap lock every launcher process shares, \
             with the exchange-only swap and the new launcher's own executable: {all}"
        );
        assert!(
            !all.contains("apply_macos_tgz("),
            "the pressed update's apply falls back to two renames, and the unattended swap must \
             never reach it: {all}"
        );
        assert!(body.iter().any(|l| l == "apply: &apply,"), "the seams are not handed that apply: {body:#?}");
    }

    // ---- the production apply: the swap lock, the stage, and who answers ------------------------

    /// The installed launcher in a folder of its own, and an archive of the one that replaces it,
    /// packed the way the packager ships it: `GeneralsX Launcher.app` at its top.
    struct Installed {
        _dir: Scratch,
        app: std::path::PathBuf,
        tgz: std::path::PathBuf,
    }

    fn installed_launcher(tag: &str) -> Installed {
        let dir = Scratch::new(tag);
        let app = dir.0.join("Apps").join("GeneralsX Launcher.app");
        let packed = dir.0.join("packed").join("GeneralsX Launcher.app");
        for (b, which) in [(&app, "old"), (&packed, "new")] {
            let macos = b.join("Contents").join("MacOS");
            std::fs::create_dir_all(&macos).expect("a stand-in bundle");
            std::fs::write(macos.join("marker"), which).expect("its marker");
        }
        let tgz = dir.0.join("GeneralsXLauncher-macos-bbbbbbbbb.tar.gz");
        let packed_ok = std::process::Command::new("tar")
            .arg("-czf")
            .arg(&tgz)
            .arg("-C")
            .arg(dir.0.join("packed"))
            .arg("GeneralsX Launcher.app")
            .status()
            .expect("tar can be run")
            .success();
        assert!(packed_ok, "the stand-in launcher could not be packed");
        Installed { _dir: dir, app, tgz }
    }

    /// Everything beside the installed launcher: the stages, and nothing else.
    fn beside(app: &std::path::Path) -> Vec<std::path::PathBuf> {
        let mut v: Vec<std::path::PathBuf> = std::fs::read_dir(app.parent().expect("a folder"))
            .expect("the folder can be read")
            .flatten()
            .map(|e| e.path())
            .filter(|p| p != app)
            .collect();
        v.sort();
        v
    }

    /// A swap lock no other launcher process holds, for the tests that are not about the lock.
    fn unheld() -> Result<Option<()>, String> {
        Ok(Some(()))
    }

    #[test]
    fn the_stage_holding_the_only_previous_launcher_is_kept() {
        let i = installed_launcher("apply-stuck");
        let tries = std::cell::Cell::new(0u32);
        let only_once = |x: &std::path::Path, y: &std::path::Path| {
            tries.set(tries.get() + 1);
            if tries.get() == 1 {
                exchanged(x, y)
            } else {
                Err("the volume refused the second exchange".to_string())
            }
        };
        let never = |_: &std::path::Path| ProbeRun::TimedOut;
        let out = apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &unheld, &only_once, &never);
        assert!(matches!(out, Err(SwapRefused::Stuck(_))), "{out:?}");
        let stages = beside(&i.app);
        assert_eq!(stages.len(), 1, "the stage holding the previous launcher is gone: {stages:?}");
        assert_eq!(
            marker(&stages[0].join("GeneralsX Launcher.app")).as_deref(),
            Some("old"),
            "the previous launcher could not be put back, so it is the only copy, and it was deleted"
        );
        assert_eq!(marker(&i.app).as_deref(), Some("new"));
    }

    #[test]
    fn the_stage_goes_once_the_new_launcher_is_kept_or_refused() {
        let i = installed_launcher("apply-kept");
        let right = |_: &std::path::Path| answers("bbbbbbbbb", 2_000);
        assert_eq!(apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &unheld, &exchanged, &right), Ok(()));
        assert_eq!(marker(&i.app).as_deref(), Some("new"));
        assert!(beside(&i.app).is_empty(), "the previous launcher outlived an answer: {:?}", beside(&i.app));

        let i = installed_launcher("apply-refused");
        let wrong = |_: &std::path::Path| answers("ccccccccc", 2_000);
        let out = apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &unheld, &exchanged, &wrong);
        assert!(matches!(out, Err(SwapRefused::Failed(_))), "{out:?}");
        assert_eq!(marker(&i.app).as_deref(), Some("old"), "the launcher the player had must be back");
        assert!(
            beside(&i.app).is_empty(),
            "the refused launcher was left beside the installed one: {:?}",
            beside(&i.app)
        );
    }

    #[test]
    fn a_swap_another_launcher_process_holds_unpacks_nothing_and_moves_nothing() {
        let i = installed_launcher("apply-busy");
        let touched = std::cell::Cell::new(false);
        let exchange = |x: &std::path::Path, y: &std::path::Path| {
            touched.set(true);
            exchanged(x, y)
        };
        let probe = |_: &std::path::Path| {
            touched.set(true);
            ProbeRun::TimedOut
        };
        let held_elsewhere = || -> Result<Option<()>, String> { Ok(None) };
        let out = apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &held_elsewhere, &exchange, &probe);
        assert!(
            matches!(&out, Err(SwapRefused::Busy(why)) if why.contains("Another GeneralsX launcher")),
            "{out:?}"
        );
        assert!(
            !touched.get(),
            "another launcher process holds the swap lock, and this one exchanged or started a \
             launcher anyway"
        );
        assert_eq!(marker(&i.app).as_deref(), Some("old"));
        assert!(
            beside(&i.app).is_empty(),
            "nothing may be unpacked while another launcher process is swapping: {:?}",
            beside(&i.app)
        );

        let no_lock = || -> Result<Option<()>, String> { Err("The state folder is read-only.".to_string()) };
        let out = apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &no_lock, &exchange, &probe);
        assert!(
            matches!(&out, Err(SwapRefused::Unavailable(why)) if why.contains("read-only")),
            "a swap lock that cannot be taken at all leaves the launcher where it is: {out:?}"
        );
        assert!(!touched.get());
        assert!(beside(&i.app).is_empty(), "{:?}", beside(&i.app));
    }

    #[test]
    fn the_swap_lock_is_held_through_the_exchange_the_answer_and_the_exchange_back() {
        /// A lock that says whether it is still held.
        struct Held(std::rc::Rc<std::cell::Cell<bool>>);
        impl Drop for Held {
            fn drop(&mut self) {
                self.0.set(false);
            }
        }
        let i = installed_launcher("apply-locked");
        let held = std::rc::Rc::new(std::cell::Cell::new(false));
        let take = || -> Result<Option<Held>, String> {
            held.set(true);
            Ok(Some(Held(held.clone())))
        };
        let seen = std::cell::RefCell::new(Vec::new());
        let exchange = |x: &std::path::Path, y: &std::path::Path| {
            seen.borrow_mut().push(("exchange", held.get()));
            exchanged(x, y)
        };
        let probe = |_: &std::path::Path| {
            seen.borrow_mut().push(("answer", held.get()));
            ProbeRun::TimedOut
        };
        let out = apply_launcher(&i.tgz, &i.app, "bbbbbbbbb", &take, &exchange, &probe);
        assert!(matches!(out, Err(SwapRefused::Failed(_))), "{out:?}");
        assert_eq!(
            *seen.borrow(),
            [("exchange", true), ("answer", true), ("exchange", true)],
            "the exchange, the new launcher's answer and the exchange back must all run under the \
             swap lock"
        );
        assert!(!held.get(), "the swap lock must be let go once the stage is settled");
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn a_swapped_in_bundle_is_started_by_the_name_this_launcher_runs_under() {
        let s = Scratch::new("bundle-exe");
        let bundle = s.0.join("GeneralsX Launcher.app");
        let macos = bundle.join("Contents").join("MacOS");
        std::fs::create_dir_all(&macos).expect("a stand-in bundle");
        let exe = std::env::current_exe().expect("this test binary");
        let name = exe.file_name().expect("it has a name").to_string_lossy().into_owned();
        stand_in(&macos, &name, &format!("echo '{}'", identity("bbbbbbbbb", 2_000).trim()));
        assert_eq!(
            probe_verdict(&run_identify(&bundle_executable(&bundle), PROBE_TIMEOUT), "bbbbbbbbb"),
            Ok(()),
            "the new bundle must be started through the executable named as this launcher's own - \
             the packager gives every build the same name - or no update is ever kept"
        );
    }

    /// What turns the next test into a launcher process of its own.
    const IDENTIFY_CHILD: &str = "--identify-child";

    /// THE OTHER HALF of `a_launcher_started_with_the_identify_flag_answers_and_ends_with_code_0`,
    /// and nothing on its own: started by that test with `IDENTIFY_CHILD` on its command line, it
    /// does what `main` does first. Whether the identify flag is on that command line is that
    /// test's choice.
    #[test]
    fn a_launcher_process_that_may_be_asked_who_it_is() {
        if !std::env::args().any(|a| a == IDENTIFY_CHILD) {
            return;
        }
        if identify_requested() {
            identify_and_exit();
        }
    }

    /// A LAUNCHER ASKED WHO IT IS ANSWERS, AND ONE THAT IS NOT DOES NOT, read off a real process:
    /// this test binary started again, running only the test above, with the identify flag and
    /// without it. `a_swapped_in_launcher_answers_before_anything_else_runs` holds where `main`
    /// asks; this holds that the asking reads the real command line, and that the answer ends with
    /// exit code 0 - the only ending the swap keeps a launcher for.
    #[test]
    fn a_launcher_started_with_the_identify_flag_answers_and_ends_with_code_0() {
        let run = |flag: Option<&str>| {
            let out = std::process::Command::new(std::env::current_exe().expect("this test binary"))
                .args(["--exact", "--nocapture", "--"])
                .arg("self_update::tests::a_launcher_process_that_may_be_asked_who_it_is")
                .arg(IDENTIFY_CHILD)
                .args(flag)
                .stdin(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .output()
                .expect("the process can be started");
            (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned())
        };
        let answer = |stdout: &str| {
            stdout
                .lines()
                .filter_map(|l| serde_json::from_str::<serde_json::Value>(l).ok())
                .find(|v| v.get("gxidentify").is_some())
        };
        let (code, stdout) = run(Some(IDENTIFY_FLAG));
        let said = answer(&stdout)
            .unwrap_or_else(|| panic!("a launcher started with {IDENTIFY_FLAG} did not answer; it ended with {code:?}"));
        assert_eq!(
            said.get("commit").and_then(|c| c.as_str()),
            Some(own_build().commit9),
            "it must answer with its own commit: {said}"
        );
        assert_eq!(code, Some(0), "an answer must end with exit code 0, or the swap refuses it: {stdout:?}");

        let (code, stdout) = run(None);
        assert!(
            answer(&stdout).is_none(),
            "a launcher started WITHOUT the flag answered and ended, which every ordinary launch \
             would do too: {stdout:?}"
        );
        assert_eq!(code, Some(0), "a launcher started without the flag must run its test and end: {stdout:?}");
    }

    /// THE BACKOFF, READ WHERE PRODUCTION KEEPS IT. `run_self_update`'s tests hold what it does with
    /// `last_failed` and `failed`; this holds that production hands it the file's own read and
    /// write, not a stand-in. Either one missing brings back a download of the same failed launcher
    /// on every launch.
    #[test]
    fn production_reads_and_writes_the_backoff_file() {
        let src = include_str!("self_update.rs");
        let prod = &src[..src.find("#[cfg(test)]\nmod tests {").expect("the test module is gone")];
        assert!(
            prod.lines().any(|l| l == "const SELF_UPDATE_BACKOFF: &str = \"gxselfupdate-backoff.txt\";"),
            "the backoff is kept in gxselfupdate-backoff.txt, beside gxselfupdate.txt"
        );
        assert_eq!(
            production_body("fn read_backoff() -> Option<(u64, String)> {"),
            ["rel::read_cached(SELF_UPDATE_BACKOFF, 4096).and_then(|(_, t)| parse_backoff(&t))"],
            "the backoff is not read from its file, through the one parse a test holds"
        );
        let seams = production_body("fn with_production_seams<R>(");
        for line in [
            "let last_failed = read_backoff;",
            "let _ = rel::write_cache_atomic(SELF_UPDATE_BACKOFF, &backoff_line(at, artifact));",
            "last_failed: &last_failed,",
            "failed: &failed,",
        ] {
            assert!(
                seams.iter().any(|l| l == line),
                "production's seams have no `{line}`, so a failed launcher is downloaded again on \
                 every launch: {seams:#?}"
            );
        }
    }

    /// EVERY REPORT IS KEPT FOR A PAGE OPENED LATER, AND SENT UNDER THE NAME THE HOME SCREEN LISTENS
    /// FOR. `release_self_update_status` answers from `SELF_UPDATE_STATUS`, and MainWeb listens for
    /// `SELF_UPDATE_EVENT` (main.rs's tests pin the listener); `publish` is where both happen.
    #[test]
    fn every_report_is_kept_for_a_later_page_and_sent_as_the_event_the_home_screen_hears() {
        assert_eq!(
            production_body("fn publish(app: &tauri::AppHandle, s: SelfUpdateStatusDto) {"),
            [
                "use tauri::Emitter;",
                "if let Ok(mut last) = SELF_UPDATE_STATUS.lock() {",
                "*last = Some(s.clone());",
                "}",
                "let _ = app.emit(SELF_UPDATE_EVENT, s);",
            ],
            "a report must be kept for a page that opens later and sent as the event MainWeb hears"
        );
        let status = production_body("pub fn release_self_update_status() -> SelfUpdateStatusDto {");
        assert_eq!(
            status.first().map(String::as_str),
            Some("SELF_UPDATE_STATUS"),
            "the status command must answer from what publish keeps: {status:#?}"
        );
    }

    /// THE SWAP LOCK PRODUCTION TAKES IS ONE FILE FOR EVERY LAUNCHER PROCESS OF THIS ACCOUNT, in the
    /// state directory. Named after a process, or kept where another launcher does not look, it
    /// would lock nothing.
    #[test]
    fn production_takes_the_one_swap_lock_in_the_state_directory() {
        assert_eq!(SWAP_LOCK_FILE, "gxselfupdate.lock");
        assert_eq!(
            production_body("fn take_swap_lock() -> Result<Option<SwapLock>, String> {"),
            [
                "let path = rel::cache_path(SWAP_LOCK_FILE)",
                ".ok_or(\"There is no per-user state directory to hold the launcher's swap lock.\")?;",
                "try_swap_lock(&path)",
            ],
            "production's swap lock must be SWAP_LOCK_FILE in the state directory, taken without \
             waiting"
        );
    }
}
