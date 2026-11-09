//! The launcher's REAL entry point, `shippedmaps::install_from_payload()`, in a process of its own (it reads process-wide
//! env). Stage a payload with scripts/build/lib-overlay.sh's gx_overlay_stage_shipped_maps, then:
//!   GENERALSX_PATCH_PATH=<stage>/GeneralsXPatch GX_USERDATA_DIR=<empty dir> cargo test -p gxio --test shipped_maps_install
//! Without both variables it does nothing (so a plain `cargo test` never writes anywhere).

#[test]
fn install_puts_the_staged_maps_in_the_userdata_maps_folder() {
    let (Some(patch), Some(ud)) = (std::env::var_os("GENERALSX_PATCH_PATH"), std::env::var_os("GX_USERDATA_DIR")) else {
        return;
    };
    let (patch, ud) = (std::path::PathBuf::from(patch), std::path::PathBuf::from(ud));
    let staged: Vec<_> = std::fs::read_dir(patch.join(gxio::shippedmaps::DIR)).expect("a staged payload").flatten().collect();
    assert!(!staged.is_empty(), "nothing staged");
    let r = gxio::shippedmaps::install_from_payload(&patch).expect("runs");
    assert!(r.failed.is_empty(), "{:?}", r.failed);
    assert_eq!(r.installed.len(), staged.len(), "{r:?}");
    for e in staged {
        let n = e.file_name().to_string_lossy().to_string();
        assert!(ud.join("Maps").join(&n).join(format!("{n}.map")).is_file(), "{n}");
    }
    // A second launch changes nothing.
    let again = gxio::shippedmaps::install_from_payload(&patch).expect("runs");
    assert_eq!((again.installed.len(), again.replaced.len(), again.unchanged.len()), (0, 0, r.installed.len()));
}
