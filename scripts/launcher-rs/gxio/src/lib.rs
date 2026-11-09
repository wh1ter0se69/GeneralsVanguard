//! The launcher's I/O layer. gxcore is deliberately pure; everything that touches a disk,
//! a network or a process lives here.
pub mod bigfile;
pub mod ciguard;
pub mod commandmap;
pub mod commandset;
pub mod crashlogs;
pub mod engine;
pub mod enginestamp;
pub mod filesums;
pub mod game;
pub mod keybinds;
pub mod mappreview;
pub mod netdiag;
pub mod options;
pub mod release;
pub mod renderceiling;
pub mod sagepatch;
pub mod setupchoices;
pub mod shippedmaps;
pub mod state;
// UNIX ONLY, and that is the design rather than a limitation. The Steam fetch exists for players
// who own Zero Hour but have no local copy - a macOS and Linux problem. Windows has no setup page
// at all (cmd_setup::page_for_platform returns None) because its installer already knows where the
// data is. This module uses unix file modes, flock and getuid throughout; compiling it on Windows
// meant either stubbing all of that for a platform with no way to reach it, or admitting the
// scope. Cross-model review found it broke the Windows launcher build outright.
#[cfg(unix)]
pub mod steam;
pub mod text;
pub mod update;
pub use keybinds::{keybinds_path, load_from, save_to, Keybinds, SHOW_INDICATORS_KEY};
pub use state::state_dir;
pub use update::{locate_target, target_for_exe, Target};
