// The commit stamp, and the one cargo directive that makes it a genuine staleness gate.
//
// The packager passes GX_COMMIT and then asserts that the binary reports that same commit back
// through --gx-selftest. That assertion is only worth anything if a *new* GX_COMMIT actually
// causes a rebuild. Cargo does not re-run a build script when an env var it was never told about
// changes, so without `cargo:rerun-if-env-changed=GX_COMMIT` the second build at a new commit
// keeps the first commit compiled in, and the gate reports "the build did not relink" about a
// build that relinked perfectly. It fails safe, but it blocks the pipeline while pointing at the
// wrong cause - which is worse than not having the gate, because it burns a debugging session.
//
// The `git rev-parse HEAD` fallback is for a developer running `cargo run` by hand. The packager
// always sets GX_COMMIT, so the fallback never runs in a packaged build; that is deliberate,
// because a stamp that re-derives itself from the working tree could never disagree with the tree
// and so could never detect staleness at all.

include!("build_env.rs");

fn main() {
    println!("cargo:rerun-if-env-changed=GX_COMMIT");

    let commit = match std::env::var("GX_COMMIT") {
        Ok(c) if !c.trim().is_empty() => c.trim().to_string(),
        _ => std::process::Command::new("git")
            .args(["rev-parse", "HEAD"])
            .output()
            .ok()
            .filter(|o| o.status.success())
            .and_then(|o| String::from_utf8(o.stdout).ok())
            .map(|s| s.trim().to_string())
            .unwrap_or_else(|| "000000000".to_string()),
    };

    // Nine characters, matching gx_commit9 in lib-gate.sh and the `commit` field of the selftest
    // contract. Truncating here rather than at the print site means there is exactly one place
    // the width is decided.
    let commit9: String = commit.chars().take(9).collect();
    println!("cargo:rustc-env=GX_COMMIT9={commit9}");

    // THE COMMIT TIME, and the self-update fixture's one gate. Both rules live in build_env.rs,
    // where a test holds them; a refusal stops the build here rather than shipping a launcher that
    // never updates itself, or one that honours a test stand-in.
    let epoch = std::env::var("GX_COMMIT_EPOCH").ok();
    let fixture = std::env::var("GX_SELFUPDATE_FIXTURE_BUILD").ok();
    match stamp_directives(epoch.as_deref(), fixture.as_deref()) {
        Ok(lines) => lines.iter().for_each(|l| println!("{l}")),
        Err(why) => panic!("{why}"),
    }

    tauri_build::build()
}
