// What build.rs tells cargo about this binary's commit time and the self-update fixture.
//
// ITS OWN FILE, AND PURE, so that build.rs and a unit test compile the SAME function: build.rs is
// `include!("build_env.rs")` and so is `self_update::tests::build_env`. A build script has no test
// harness of its own, and the two rules below are the kind that fail silently - a launcher that
// never updates itself, or one that honours a test stand-in - so they are held by tests instead of
// by reading.
//
// GX_COMMIT_EPOCH. `git show -s --format=%ct HEAD`, exported by package-launcher-macos.sh beside
// GX_COMMIT. It is the commit's OWN time, the clock the release manifest's ordinal is on
// (gx-manifest.py writes the release commit's %ct), so the launcher can tell whether a published
// launcher is newer than itself. A build without it is a development build and never replaces
// itself. Anything that is not a positive number of seconds stops the build: a wrong value would
// compare every release against the wrong clock, and nobody would find out until an update failed
// to arrive. "0" is refused with the rest. It is a number of seconds, but compiled in it would
// read as the oldest build there is, and a launcher that believes that installs whatever is
// published, downgrades included.
//
// GX_SELFUPDATE_FIXTURE_BUILD=1. Builds in the stand-in that hands a scratch copy of the launcher a
// "newer launcher" from a local folder, so the silent swap can be exercised in the real app
// without publishing anything. REFUSED OUTRIGHT together with GX_COMMIT_EPOCH: every build the
// packager makes carries a commit time, so no packaged launcher can ever contain the stand-in.
// self_update.rs holds the same rule a second time, at compile time, for a cfg switched on some
// other way than through this variable.
//
// `rerun-if-env-changed` for BOTH, and it is load-bearing, for the reason build.rs gives for
// GX_COMMIT: without it cargo keeps the first answer it computed. A fixture build followed by a
// packaged build at the same commit in the same target directory would otherwise ship the fixture's
// answer - no commit time, and so a launcher that never updates.
pub(crate) fn stamp_directives(
    epoch: Option<&str>,
    fixture: Option<&str>,
) -> Result<Vec<String>, String> {
    let mut out = vec![
        "cargo:rerun-if-env-changed=GX_COMMIT_EPOCH".to_string(),
        "cargo:rerun-if-env-changed=GX_SELFUPDATE_FIXTURE_BUILD".to_string(),
        "cargo:rustc-check-cfg=cfg(gx_selfupdate_fixture)".to_string(),
    ];

    let epoch = match epoch.map(str::trim) {
        None | Some("") => None,
        Some(e) => {
            let seconds = e.len() <= 12 && e.bytes().all(|b| b.is_ascii_digit());
            let positive = e.bytes().any(|b| b != b'0');
            if !(seconds && positive) {
                return Err(format!(
                    "GX_COMMIT_EPOCH is `{e}`, which is not a commit time: a positive number of \
                     seconds. The packager sets it from `git show -s --format=%ct HEAD`."
                ));
            }
            Some(e)
        }
    };

    match fixture.map(str::trim) {
        None | Some("") => {}
        Some("1") => {
            if let Some(e) = epoch {
                return Err(format!(
                    "GX_SELFUPDATE_FIXTURE_BUILD=1 together with GX_COMMIT_EPOCH={e}: a launcher \
                     that carries a commit time is a packaged launcher, and a packaged launcher \
                     must never honour the self-update fixture. Build the fixture without \
                     GX_COMMIT_EPOCH."
                ));
            }
            out.push("cargo:rustc-cfg=gx_selfupdate_fixture".to_string());
        }
        Some(other) => {
            return Err(format!(
                "GX_SELFUPDATE_FIXTURE_BUILD is `{other}`; the only value it takes is 1."
            ))
        }
    }

    if let Some(e) = epoch {
        out.push(format!("cargo:rustc-env=GX_COMMIT_EPOCH={e}"));
    }
    Ok(out)
}
