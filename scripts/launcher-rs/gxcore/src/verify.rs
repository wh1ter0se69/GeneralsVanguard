use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use ed25519_dalek::{Signature, VerifyingKey};

use crate::error::GxError;
use crate::wire::split_wire;

/// The trust anchor, verbatim from gxlaunch.c:107-112. Compiled in rather than configured: a
/// manifest is only as trustworthy as the key that checks it, and a key read from disk is a key
/// an attacker can replace.
pub const GX_MANIFEST_PUBKEY: [u8; 32] = [
    0x28, 0xc8, 0xd4, 0x77, 0x3c, 0xef, 0xa3, 0x27,
    0x45, 0xc6, 0x18, 0x99, 0xde, 0x54, 0x77, 0x90,
    0x65, 0x23, 0x66, 0x68, 0xdf, 0x80, 0xed, 0x33,
    0x1b, 0xa4, 0x93, 0x1e, 0x80, 0x26, 0x20, 0xc4,
];

/// GeneralsX @feature Claude 29/10/2026 S1-3 relay-list-v2: THE RANKED KEY SET (modernize/PLAN_smooth-1.md §3.4).
///
/// K1 < K2 < K3. K1 is the key above. K2 and K3 are its successors: the operator generates them offline
/// (`gx-manifest.py keygen --successor k2`) and pastes `pubkey --key k2 --c-array` here, into gxlaunch.c's
/// GX_MANIFEST_KEYS and into GXRelayList.cpp, BEFORE a release that must be able to rotate. Until then the
/// slots are all zero and `verify` skips them explicitly - never "verifies" against them. A document's RANK is
/// the slot whose key verifies it; a launcher keeps the higher-ranked document and never replaces it with a
/// lower-ranked one (`supersedes`), so one K2 document retires K1 on that machine.
pub const GX_MANIFEST_KEYS: [[u8; 32]; 3] = [GX_MANIFEST_PUBKEY, [0u8; 32], [0u8; 32]];

/// A payload that has been verified. The fields are private, so `payload()` is the only way to
/// reach the bytes and there is no route to the parser that skips verification.
///
/// The derives are comparison and printing only. Neither is a constructor, so the type boundary
/// survives them: outside this module the sole way to obtain a `Verified` is to call `verify`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Verified<'a>(&'a str, u8);

impl<'a> Verified<'a> {
    pub fn payload(&self) -> &'a str {
        self.0
    }

    /// Which pinned key verified it: 1 = K1, 2 = K2, 3 = K3.
    pub fn rank(&self) -> u8 {
        self.1
    }

    /// Test-only. Lets manifest.rs exercise the parser without signing every fixture.
    /// Never compiled into a release build.
    #[cfg(test)]
    pub(crate) fn for_test(payload: &'a str) -> Self {
        Verified(payload, 1)
    }
}

/// Verify a wire document against the pinned ranked keys. Base64url, unpadded, 64 bytes. gxlaunch.c:660-679.
pub fn verify(wire: &str) -> Result<Verified<'_>, GxError> {
    verify_with(&GX_MANIFEST_KEYS, wire)
}

/// `verify` against an explicit ranked key set (slot i = rank i + 1). An all-zero slot is unprovisioned and
/// skipped. Public so the launcher's tests can pin the shared TEST keys (scripts/test/relay-list-fixtures).
pub fn verify_with<'a>(keys: &[[u8; 32]], wire: &'a str) -> Result<Verified<'a>, GxError> {
    let s = split_wire(wire)?;

    let raw = URL_SAFE_NO_PAD
        .decode(s.signature.as_bytes())
        .map_err(|_| GxError::BadSignatureEncoding)?;
    let raw: [u8; 64] = raw.try_into().map_err(|_| GxError::BadSignatureEncoding)?;
    let sig = Signature::from_bytes(&raw);

    for (i, k) in keys.iter().enumerate().take(3) {
        if k.iter().all(|b| *b == 0) {
            continue;
        }
        let Ok(key) = VerifyingKey::from_bytes(k) else { continue };
        // verify_strict, not verify: it rejects small-order public keys and non-canonical S, which is
        // the closest match to the accept set GXEd25519.h states.
        if key.verify_strict(s.payload.as_bytes(), &sig).is_ok() {
            return Ok(Verified(s.payload, (i + 1) as u8));
        }
    }
    Err(GxError::SignatureMismatch)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::error::GxError;

    /// The live document, fetched 2026-08-19. Its signature is real and must verify against the
    /// compiled-in anchor. If this test ever fails, either the key rotated or the port is wrong -
    /// and those need very different responses, so this fixture is checked in deliberately.
    ///
    /// It is an OLDER document than whatever is published today, and that is fine: a signature
    /// does not expire. Never "fix" a failure here by pasting in the current manifest - that
    /// swaps a test of the trust chain for a test of the network.
    const LIVE: &str = "2|1787151908|0|1kuBzZqh|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-d64b81cd9.zip,WXnyJwUE3cqiaE-WAATNfg,4492409;win32,gxrt_66dd5434.zip,Tk7OXg2W6aSEcfFmLGPGeg,63948459;any,gxreq_58c3de93.zip,QP8skZClYF7Lx2vsIraAwA,29204191;win32,gxopt_3ae6628a.zip,FVVs51YS3vq6i5PuqOctXw,21109602|ecrTymzgRPT2fmz-vNDjdQhUaE98w_6Pb5JpvBDGiQ_TsJd8Rleo0267tgrYyEnLcoz4RX5ON3YxEMD5ZfapDQ";

    #[test]
    fn the_live_manifest_verifies() {
        let v = verify(LIVE).expect("the live manifest must verify");
        assert!(v.payload().starts_with("2|1787151908|"));
        assert!(!v.payload().contains("ecrTymzg"), "the signature must not be inside the payload");
    }

    #[test]
    fn one_flipped_payload_byte_is_refused() {
        // Change the ordinal. Everything else is untouched, so only the signature can catch it.
        let tampered = LIVE.replacen("2|1787151908|", "2|1787151909|", 1);
        assert_eq!(verify(&tampered), Err(GxError::SignatureMismatch));
    }

    /// The shared S1-3 fixtures' TEST keys (scripts/test/relay-list-fixtures.py --check reads these markers).
    // GX_RELAYLIST_TEST_K1 79379e76ed44c7a1633c9b981396584ec3041a57f7c5f2ec14e65118159725b7
    // GX_RELAYLIST_TEST_K2 3b83f149150388400756c2523a72e85bf50f46e584d5a4debb75180bbae1bd7e
    // GX_RELAYLIST_TEST_K3 e69ff53e81b45a15680613c5ba2ff653cdb01a02fbfa56eec47a4e06fd360635
    pub(crate) fn test_keys() -> [[u8; 32]; 3] {
        let hex = |h: &str| -> [u8; 32] {
            let mut k = [0u8; 32];
            for i in 0..32 {
                k[i] = u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap();
            }
            k
        };
        [
            hex("79379e76ed44c7a1633c9b981396584ec3041a57f7c5f2ec14e65118159725b7"),
            hex("3b83f149150388400756c2523a72e85bf50f46e584d5a4debb75180bbae1bd7e"),
            hex("e69ff53e81b45a15680613c5ba2ff653cdb01a02fbfa56eec47a4e06fd360635"),
        ]
    }

    #[test]
    fn the_shared_fixtures_verify_with_their_rank() {
        let k = test_keys();
        assert_eq!(verify_with(&k, crate::fixtures::K1_HOME).unwrap().rank(), 1);
        assert_eq!(verify_with(&k, crate::fixtures::K2_HOME).unwrap().rank(), 2);
        assert_eq!(verify_with(&k, crate::fixtures::K3_HOME).unwrap().rank(), 3);
        assert_eq!(verify_with(&k, crate::fixtures::ROGUE_HOME), Err(GxError::SignatureMismatch));
    }

    #[test]
    fn an_unprovisioned_slot_verifies_nothing() {
        // A K1-only build (the production table today) refuses a K2 document: zero slots are skipped.
        let k = test_keys();
        let k1_only = [k[0], [0u8; 32], [0u8; 32]];
        assert_eq!(verify_with(&k1_only, crate::fixtures::K2_HOME), Err(GxError::SignatureMismatch));
        assert_eq!(verify(crate::fixtures::K1_HOME), Err(GxError::SignatureMismatch), "test keys are not production keys");
    }

    #[test]
    fn one_flipped_signature_byte_is_refused() {
        let mut t = LIVE.to_string();
        let last = t.pop().unwrap();
        t.push(if last == 'A' { 'B' } else { 'A' });
        assert_eq!(verify(&t), Err(GxError::SignatureMismatch));
    }
}
