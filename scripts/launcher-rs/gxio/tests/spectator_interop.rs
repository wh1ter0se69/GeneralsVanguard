//! The launcher and the engine's in-game customise panel write the SAME GXHud.ini. Neither may clobber the other.
//!
//! This drives the engine's REAL parser and writer (`Core/GameEngine/Include/GameClient/GXSpectatorHud.h`, compiled by
//! `scripts/test/spectator-interop-engine.cpp`) against the launcher's real writer, in both orders. It needs a C++17
//! compiler, so it is `#[ignore]`d from the default run:
//!
//!   cargo test -p gxio --test spectator_interop -- --ignored
use gxio::options as opt;
use std::path::{Path, PathBuf};
use std::process::Command;

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(3).expect("repo root").to_path_buf()
}

fn build_harness(dir: &Path) -> PathBuf {
    let root = repo_root();
    let exe = dir.join("engine-harness");
    let st = Command::new("c++")
        .args(["-std=c++17", "-I"])
        .arg(root.join("Core/GameEngine/Include"))
        .arg(root.join("scripts/test/spectator-interop-engine.cpp"))
        .arg("-o")
        .arg(&exe)
        .status()
        .expect("a C++ compiler (c++) is needed for this test");
    assert!(st.success(), "the engine harness did not compile");
    exe
}

fn run(exe: &Path, mode: &str, file: &Path) -> String {
    let o = Command::new(exe).arg(mode).arg(file).output().unwrap();
    assert!(o.status.success(), "{mode}: {}", String::from_utf8_lossy(&o.stderr));
    String::from_utf8(o.stdout).unwrap()
}

#[test]
#[ignore = "needs a C++ compiler; run with --ignored"]
fn the_launcher_and_the_engine_do_not_clobber_each_other() {
    let dir = std::env::temp_dir().join(format!("gx-spec-interop-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = build_harness(&dir);
    let f = dir.join("GXHud.ini");
    // A player's file: the launcher's anchor + chat fade, a comment, and a line this code knows nothing about.
    std::fs::write(&f, "; mine\nHudAnchorX = 20\nMessageDelaySeconds = 12\nSomethingElse = 7\n").unwrap();

    // 1) LAUNCHER writes -> ENGINE reads.
    let e = opt::GxSpectatorEdit {
        ui: Some("generalsx".into()),
        opacity: Some(80),
        hide_dead: Some(false),
        show: vec![("Names".into(), false), ("UnitsBuildings".into(), true)],
        // GeneralsX @tooling Claude 11/10/2026 the while-playing switches 93e617e0f added to the struct: left
        // unset (None writes no key), so this round-trip still checks exactly the keys it was written for.
        player_strip: None,
        player_tab_board: None,
    };
    opt::gxhud_save(&f, &e.pairs().unwrap(), &[]).unwrap();
    assert_eq!(run(&exe, "parse", &f).trim(), "ui=generalsx opacity=80 hide=0 show=1011111111111", "engine read the launcher's keys");

    // 2) ENGINE writes (customise panel: Cash off, opacity +5, hide dead) -> LAUNCHER reads, and kept its own lines.
    std::fs::write(&f, run(&exe, "edit", &f)).unwrap();
    let after_engine = std::fs::read_to_string(&f).unwrap();
    for keep in ["; mine\n", "HudAnchorX = 20\n", "MessageDelaySeconds = 12\n", "SomethingElse = 7\n"] {
        assert!(after_engine.contains(keep), "engine write lost {keep:?}:\n{after_engine}");
    }
    let s = opt::gxspectator_load_from(&f);
    assert!(s.generalsx && s.opacity == 85 && s.hide_dead && !s.show[2] && !s.show[1] && s.show[6], "{s:?}\n{after_engine}");
    assert_eq!(opt::gxhud_load_from(&f).anchor_x, Some(20));

    // 3) LAUNCHER writes again on top of the engine's file: only its own keys move, the engine's choices stay.
    let e2 = opt::GxSpectatorEdit { opacity: Some(60), ..Default::default() };
    opt::gxhud_save(&f, &e2.pairs().unwrap(), &[]).unwrap();
    let g = opt::GxHudEdit { anchor_y: Some(9), ..Default::default() };
    opt::gxhud_save(&f, &g.pairs(), &g.removals()).unwrap();
    assert_eq!(run(&exe, "parse", &f).trim(), "ui=generalsx opacity=60 hide=1 show=1001111111111", "engine read the merged file");
    assert!(std::fs::read_to_string(&f).unwrap().contains("SomethingElse = 7\n"));
    let _ = std::fs::remove_dir_all(&dir);
}
