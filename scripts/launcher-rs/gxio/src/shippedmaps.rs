//! The maps GeneralsX ships, copied into the player's own `<userdata>/Maps` on every launch.
//!
//! GeneralsX @feature Claude 30/09/2026 The owner: "ship the two maps in the launcher for all clients".
//! Design: gx-handoff maps/P6/SHIP_PATH.md, Option B.
//!
//! THE PAYLOAD CARRIES THEM AS LOOSE FILES in `<GeneralsXPatch>/ShippedMaps/<Name>/`, inside the optional
//! tier, so no new artifact role exists. They are inert there: the engine's overlay scan matches `*.big`
//! only. Maps are outside `dataID` and the sim digest (SimulationId.cpp lists "Maps" as deliberately not in
//! dataID), so nothing here can refuse a join - which is also why they must never ride the `required` tier.
//!
//! THE ENGINE NEEDS NO CHANGE: it scans `<userdata>/Maps` from disk on every launch (MapUtil.cpp) as
//! unofficial maps, and a host transfers them to anyone who lacks them. This only saves the manual unzip.
//!
//! WHAT IT WILL AND WILL NOT TOUCH. A shipped map is a folder `<Name>` holding `<Name>.map` plus its
//! sidecar files. It is installed when `<userdata>/Maps/<Name>/<Name>.map` is missing or its sha256
//! differs from the shipped one, and only the shipped folder's own file names are written. A folder the
//! player made under any other name is never opened; nothing is ever deleted. Each file lands by a temp
//! name and a rename, so a crash or a full disk leaves the old map, not half of the new one.

use std::fs;
use std::path::{Path, PathBuf};

/// The folder inside the overlay directory that holds the shipped map folders.
pub const DIR: &str = "ShippedMaps";

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Report {
    pub installed: Vec<String>,
    pub replaced: Vec<String>,
    pub unchanged: Vec<String>,
    /// One player-readable line per map that could not be installed. The others still are.
    pub failed: Vec<String>,
}

/// The launcher's artifact hash: SHA-256 cut to 22 base64 characters (128 bits) - plenty to tell two map files apart.
fn sha256_file(p: &Path) -> Result<String, String> {
    use std::io::Read;
    let mut f = fs::File::open(p).map_err(|e| format!("{}: {e}", p.display()))?;
    let mut h = gxcore::ArtifactHasher::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("{}: {e}", p.display()))?;
        if n == 0 {
            return Ok(h.finish());
        }
        h.update(&buf[..n]);
    }
}

/// A folder name that is one plain path segment: it becomes a folder under the player's Maps.
fn safe_name(n: &str) -> bool {
    !n.is_empty() && n != "." && n != ".." && !n.contains(['/', '\\', ':', '\0'])
}

/// Copy `src` over `dst` through a temp file beside it.
fn copy_atomic(src: &Path, dst: &Path) -> Result<(), String> {
    let tmp = dst.with_extension(format!("{}.gxtmp", dst.extension().and_then(|e| e.to_str()).unwrap_or("")));
    let r = fs::copy(src, &tmp).and_then(|_| fs::rename(&tmp, dst));
    if let Err(e) = r {
        let _ = fs::remove_file(&tmp);
        return Err(format!("{}: {e}", dst.display()));
    }
    Ok(())
}

/// Install every shipped map folder under `src_root` into `maps` (the player's `<userdata>/Maps`).
/// `Err` only when `src_root` itself cannot be read; a map that cannot be written goes in `failed`.
pub fn install_from(src_root: &Path, maps: &Path) -> Result<Report, String> {
    let mut rep = Report::default();
    let mut dirs: Vec<(String, PathBuf)> = fs::read_dir(src_root)
        .map_err(|e| format!("shipped maps: {}: {e}", src_root.display()))?
        .flatten()
        // symlink_metadata: a link is not a shipped folder, and following one leaves the payload.
        .filter(|e| e.path().symlink_metadata().map(|m| m.is_dir()).unwrap_or(false))
        .filter_map(|e| Some((e.file_name().to_str()?.to_string(), e.path())))
        .filter(|(n, _)| safe_name(n))
        .collect();
    dirs.sort();
    for (name, dir) in dirs {
        let map_name = format!("{name}.map");
        let src_map = dir.join(&map_name);
        if !src_map.is_file() {
            continue; // not a map folder; not ours to interpret
        }
        let dest = maps.join(&name);
        let dest_map = dest.join(&map_name);
        let want = match sha256_file(&src_map) {
            Ok(h) => h,
            Err(e) => {
                rep.failed.push(format!("map {name}: could not read the shipped copy ({e})"));
                continue;
            }
        };
        let had = dest_map.is_file();
        if had && sha256_file(&dest_map).map(|h| h == want).unwrap_or(false) {
            rep.unchanged.push(name);
            continue;
        }
        // Only our own file names, regular files only, the .map last so an interrupted install leaves
        // the old .map (and so the same sha mismatch) rather than a new .map beside stale sidecars.
        let mut files: Vec<PathBuf> = match fs::read_dir(&dir) {
            Ok(rd) => rd
                .flatten()
                .map(|e| e.path())
                .filter(|p| p.symlink_metadata().map(|m| m.is_file()).unwrap_or(false))
                .collect(),
            Err(e) => {
                rep.failed.push(format!("map {name} was not installed: cannot list the shipped copy ({e})"));
                continue;
            }
        };
        files.sort_by_key(|p| p == &src_map);
        let done = fs::create_dir_all(&dest)
            .map_err(|e| format!("{}: {e}", dest.display()))
            .and_then(|_| {
                files.iter().try_for_each(|f| match f.file_name() {
                    Some(n) => copy_atomic(f, &dest.join(n)),
                    None => Ok(()),
                })
            });
        match done {
            Ok(()) if had => rep.replaced.push(name),
            Ok(()) => rep.installed.push(name),
            Err(e) => rep.failed.push(format!("map {name} was not installed: cannot write {e}")),
        }
    }
    Ok(rep)
}

/// The launcher's PLAY-time call, given the install's overlay folder (`GameStatus::patch_dir`): its shipped
/// maps into this player's Maps folder. No user data folder, or no `ShippedMaps` in it (an older payload): nothing.
pub fn install_from_payload(patch_dir: &Path) -> Result<Report, String> {
    let Some(ud) = crate::release::engine_user_data_dir() else { return Ok(Report::default()) };
    let src = patch_dir.join(DIR);
    if !src.is_dir() {
        return Ok(Report::default());
    }
    install_from(&src, &ud.join("Maps"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gx-shipmaps-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }
    fn put(p: &Path, body: &str) {
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, body).unwrap();
    }
    fn ship(root: &Path, name: &str, map: &str) {
        put(&root.join(name).join(format!("{name}.map")), map);
        put(&root.join(name).join(format!("{name}.tga")), "tga");
    }

    #[test]
    fn missing_is_installed_same_is_untouched_different_is_replaced() {
        let d = scratch("cycle");
        let (src, maps) = (d.join("ship"), d.join("Maps"));
        ship(&src, "Crater", "v1");
        let r = install_from(&src, &maps).unwrap();
        assert_eq!((r.installed.as_slice(), r.failed.len()), (&["Crater".to_string()][..], 0));
        assert_eq!(fs::read_to_string(maps.join("Crater/Crater.map")).unwrap(), "v1");
        assert_eq!(fs::read_to_string(maps.join("Crater/Crater.tga")).unwrap(), "tga");

        // Same sha: nothing is written - a sidecar the player edited stays edited.
        put(&maps.join("Crater/Crater.tga"), "mine");
        let r = install_from(&src, &maps).unwrap();
        assert_eq!(r.unchanged, ["Crater"]);
        assert_eq!(fs::read_to_string(maps.join("Crater/Crater.tga")).unwrap(), "mine");

        // Different sha: replaced, .map and sidecars.
        ship(&src, "Crater", "v2");
        let r = install_from(&src, &maps).unwrap();
        assert_eq!(r.replaced, ["Crater"]);
        assert_eq!(fs::read_to_string(maps.join("Crater/Crater.map")).unwrap(), "v2");
        assert_eq!(fs::read_to_string(maps.join("Crater/Crater.tga")).unwrap(), "tga");
        assert!(!maps.join("Crater/Crater.tga.gxtmp").exists() && !maps.join("Crater/Crater.map.gxtmp").exists());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn the_players_own_maps_and_extra_files_are_never_touched() {
        let d = scratch("own");
        let (src, maps) = (d.join("ship"), d.join("Maps"));
        ship(&src, "Crater", "v1");
        put(&maps.join("MyMap/MyMap.map"), "player");
        put(&maps.join("Crater/notes.txt"), "player note in our folder");
        // A shipped folder holding no <Name>.map, and a stray file at the root, are ignored.
        put(&src.join("Junk/readme.txt"), "x");
        put(&src.join("stray.map"), "x");
        let r = install_from(&src, &maps).unwrap();
        assert_eq!(r.installed, ["Crater"]);
        assert_eq!(fs::read_to_string(maps.join("MyMap/MyMap.map")).unwrap(), "player");
        assert_eq!(fs::read_to_string(maps.join("Crater/notes.txt")).unwrap(), "player note in our folder");
        assert!(!maps.join("Junk").exists() && !maps.join("stray.map").exists());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn an_unwritable_maps_dir_is_a_clear_failure_not_a_crash() {
        let d = scratch("ro");
        let (src, maps) = (d.join("ship"), d.join("Maps"));
        ship(&src, "Crater", "v1");
        ship(&src, "Ridges", "v1");
        // `Maps` is a FILE: create_dir_all under it fails on every platform, root included.
        put(&maps, "not a folder");
        let r = install_from(&src, &maps).unwrap();
        assert!(r.installed.is_empty());
        assert_eq!(r.failed.len(), 2, "each map reports, none aborts the others: {:?}", r.failed);
        assert!(r.failed[0].contains("Crater was not installed") && r.failed[1].contains("Ridges"));
        assert_eq!(fs::read_to_string(&maps).unwrap(), "not a folder");
        let _ = fs::remove_dir_all(&d);
    }

    #[cfg(unix)]
    #[test]
    fn a_shipped_folder_that_cannot_be_listed_is_reported_not_installed_empty() {
        use std::os::unix::fs::PermissionsExt;
        let d = scratch("nolist");
        let (src, maps) = (d.join("ship"), d.join("Maps"));
        ship(&src, "Crater", "v1");
        // x without r: the .map opens (so it hashes) but the folder cannot be listed.
        fs::set_permissions(src.join("Crater"), fs::Permissions::from_mode(0o100)).unwrap();
        let root = fs::read_dir(src.join("Crater")).is_ok(); // running as root ignores modes
        let r = install_from(&src, &maps).unwrap();
        fs::set_permissions(src.join("Crater"), fs::Permissions::from_mode(0o755)).unwrap();
        if !root {
            assert!(r.installed.is_empty() && r.failed.len() == 1, "{r:?}");
            assert!(r.failed[0].contains("cannot list"), "{:?}", r.failed);
            assert!(!maps.join("Crater").exists(), "nothing half-created");
        }
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn an_unreadable_source_is_an_error_and_names_are_one_segment() {
        let d = scratch("src");
        assert!(install_from(&d.join("absent"), &d.join("Maps")).is_err());
        for bad in ["", ".", "..", "a/b", "a\\b", "C:x"] {
            assert!(!safe_name(bad), "{bad:?}");
        }
        assert!(safe_name("Twin Ridges ZH v1"));
        let _ = fs::remove_dir_all(&d);
    }
}
