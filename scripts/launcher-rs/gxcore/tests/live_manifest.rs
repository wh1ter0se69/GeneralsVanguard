//! End-to-end against the real signed document, through the public surface only - the same route
//! the Tauri shell will take. Nothing here reaches inside the crate, so if this passes, the API
//! that ships is the API that was tested.

use gxcore::{decide, parse, verify, InstallState, Verdict};

const LIVE: &str = "2|1787151908|0|1kuBzZqh|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-d64b81cd9.zip,WXnyJwUE3cqiaE-WAATNfg,4492409;win32,gxrt_66dd5434.zip,Tk7OXg2W6aSEcfFmLGPGeg,63948459;any,gxreq_58c3de93.zip,QP8skZClYF7Lx2vsIraAwA,29204191;win32,gxopt_3ae6628a.zip,FVVs51YS3vq6i5PuqOctXw,21109602|ecrTymzgRPT2fmz-vNDjdQhUaE98w_6Pb5JpvBDGiQ_TsJd8Rleo0267tgrYyEnLcoz4RX5ON3YxEMD5ZfapDQ";

#[test]
fn a_fresh_windows_install_wants_every_windows_artifact() {
    let m = parse(&verify(LIVE).unwrap()).unwrap();
    match decide(&m, &InstallState::default(), "win32") {
        Verdict::Update(a) => {
            assert_eq!(a.len(), 4, "3 win32 rows plus the shared 'any' row");
            assert!(a.iter().all(|x| x.platform == "win32" || x.platform == "any"));
        }
        v => panic!("expected an update, got {v:?}"),
    }
}

#[test]
fn a_fresh_macos_install_wants_only_the_shared_row_today() {
    // The manifest carries no macos row yet. This test is the tripwire for that: when the macOS
    // packager lands, it fails, and the person who added the row is the person who updates it.
    let m = parse(&verify(LIVE).unwrap()).unwrap();
    match decide(&m, &InstallState::default(), "macos") {
        Verdict::Update(a) => {
            assert_eq!(a.len(), 1);
            assert_eq!(a[0].platform, "any");
        }
        v => panic!("expected an update, got {v:?}"),
    }
}

#[test]
fn a_tampered_document_never_reaches_the_parser() {
    let tampered = LIVE.replacen("win32", "linux", 1);
    assert!(verify(&tampered).is_err());
}
