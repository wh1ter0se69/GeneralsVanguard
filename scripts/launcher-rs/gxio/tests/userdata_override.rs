//! GX_USERDATA_DIR run for real, in a process of its own: the only test in this binary, so setting
//! the process-wide variable races nothing. release.rs's tests hold the rule and read the wiring;
//! this RUNS the wiring on the host it is built on. Writes only its own temp folder, removed at the end.

#[test]
fn the_override_moves_every_launcher_path_and_a_refused_one_moves_none() {
    use gxio::release::{engine_user_data_dir, USER_DATA_DIR_ENV};
    std::env::remove_var(USER_DATA_DIR_ENV);
    let (normal, normal_state) = (engine_user_data_dir(), gxio::state_dir());

    let dir = std::env::temp_dir().join(format!("gx-userdata-it-{}", std::process::id()));
    std::env::set_var(USER_DATA_DIR_ENV, &dir);
    assert_eq!(engine_user_data_dir(), Some(dir.clone()));
    assert_eq!(gxio::options::options_path(), Some(dir.join("Options.ini")));
    assert_eq!(gxio::keybinds_path(), Some(dir.join("CommandButtonHotKeys.ini")));
    assert_eq!(gxio::state_dir(), Some(dir.join("launcher")));
    assert_ne!(gxio::state_dir(), normal_state, "the launcher's own state must leave the owner's folder");
    assert!(gxio::release::refuse_install_on_test_profile().is_err(), "a test profile installs nothing");

    // An older launcher's key file stays in the game folder: taking it out of there into a test profile
    // would cost the owner's normal profile its bindings (Codex r1 of e2d7b2360, P2).
    let game = dir.join("game");
    let old = gxio::commandmap::path_in(&game);
    let bound = std::collections::BTreeMap::from([(gxio::commandmap::GAME_HUD.to_string(), "KEY_F10".to_string())]);
    gxio::commandmap::save_to(&old, &bound).expect("an old-layout key file");
    assert!(old.is_file(), "the fixture wrote nothing");
    assert_eq!(gxio::commandmap::migrate_from_game_folder(&game, &dir).expect("runs"), gxio::commandmap::Migration::Nothing);
    assert!(old.is_file(), "the game folder's key file must stay where it is");
    assert!(!gxio::commandmap::user_path(&dir).exists(), "nothing is copied into the test profile");
    let _ = std::fs::remove_dir_all(&dir);

    // Refused, as the engine refuses it: the normal folders, exactly.
    std::env::set_var(USER_DATA_DIR_ENV, "relative/dir");
    assert_eq!((engine_user_data_dir(), gxio::state_dir()), (normal.clone(), normal_state.clone()));
    std::env::set_var(USER_DATA_DIR_ENV, "");
    assert_eq!((engine_user_data_dir(), gxio::state_dir()), (normal, normal_state));
    assert!(gxio::release::refuse_install_on_test_profile().is_ok(), "a refused value is no test profile");
}
