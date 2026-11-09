use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use sha2::{Digest, Sha256};

use crate::error::GxError;
use crate::verify::Verified;

pub const MANIFEST_VERSION: i32 = 2;
pub const MAX_ARTIFACTS: usize = 16;
pub const MAX_RELAYS: usize = 8;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Relay {
    pub host: String,
    /// Empty on the wire means "the default port", not "port zero".
    pub port: Option<u16>,
    pub region: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Artifact {
    pub platform: String,
    pub name: String,
    /// base64url of the FIRST 16 BYTES of SHA-256 - 22 characters, not a full digest.
    pub hash: String,
    pub size: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub version: i32,
    pub ordinal: u64,
    pub minimum: u64,
    /// GeneralsX @feature Claude 29/10/2026 S1-3: which pinned key verified it (1 = K1). See `supersedes`.
    pub rank: u8,
    /// Field 9, the signing time (S1-3). None on a document from before it. The RELAY LIST's freshness is
    /// (rank, serial) and is the engine's business (GXRelayList.cpp); the launcher uses it only to refuse a
    /// same-ordinal re-sign that is OLDER than the one it holds.
    pub serial: Option<u64>,
    pub sha: String,
    pub relays: Vec<Relay>,
    pub base: String,
    pub artifacts: Vec<Artifact>,
}

pub fn parse(v: &Verified<'_>) -> Result<Manifest, GxError> {
    let f: Vec<&str> = v.payload().split('|').collect();
    if f.len() < 7 {
        return Err(GxError::MalformedField("too few fields"));
    }

    let version: i32 = f[0].parse().map_err(|_| GxError::MalformedField("version"))?;
    if version != MANIFEST_VERSION {
        return Err(GxError::UnsupportedVersion(version));
    }

    let ordinal = f[1].parse().map_err(|_| GxError::MalformedField("ordinal"))?;
    let minimum = f[2].parse().map_err(|_| GxError::MalformedField("minimum"))?;
    // Fields 7-9 (master, alt, serial) are appended and IGNORED by this parser except for the serial, which
    // is read when it is a plain decimal and otherwise treated as absent: the update half of the document is
    // still good, and refusing it here would be the forward-compatibility break the format exists to avoid.
    let serial = f.get(9).filter(|t| !t.is_empty() && t.len() <= 12 && t.bytes().all(|b| b.is_ascii_digit()))
        .and_then(|t| t.parse::<u64>().ok());

    let mut relays = Vec::new();
    for row in f[4].split(';').filter(|r| !r.is_empty()) {
        if relays.len() == MAX_RELAYS {
            break;
        }
        let p: Vec<&str> = row.split(',').collect();
        if p.len() != 3 {
            continue;
        }
        relays.push(Relay {
            host: p[0].to_string(),
            port: if p[1].is_empty() { None } else { p[1].parse().ok() },
            region: p[2].to_string(),
        });
    }

    let mut artifacts = Vec::new();
    for row in f[6].split(';').filter(|r| !r.is_empty()) {
        if artifacts.len() == MAX_ARTIFACTS {
            break;
        }
        let p: Vec<&str> = row.split(',').collect();
        // A row that is not exactly four parts is DROPPED, not fatal. One malformed artifact
        // must never cost a player the other three. gxlaunch.c:738-742.
        if p.len() != 4 {
            continue;
        }
        let Ok(size) = p[3].parse::<u64>() else { continue };
        artifacts.push(Artifact {
            platform: p[0].to_string(),
            name: p[1].to_string(),
            hash: p[2].to_string(),
            size,
        });
    }

    Ok(Manifest {
        version,
        ordinal,
        minimum,
        rank: v.rank(),
        serial,
        sha: f[3].to_string(),
        relays,
        base: f[5].to_string(),
        artifacts,
    })
}

/// The serial horizon: a serial more than this many seconds past the clock is treated as ABSENT by
/// `supersedes` (GXRelayList.h GX_RELAYLIST_HORIZON_S - the engine refuses such a list outright).
pub const SERIAL_HORIZON_S: u64 = 7 * 24 * 3600;

/// GeneralsX @feature Claude 29/10/2026 S1-3: may `new` replace the held document `held`, at Unix time `now`?
///
/// RANK FIRST (§3.4): a lower-ranked document never replaces a higher-ranked one, and a HIGHER-ranked one
/// always does, whatever its ordinal - that is what lets a K2 recovery document land on a machine whose K1
/// floor a stolen key has poisoned (§3.3: the floor is per rank). Within a rank, the existing rule: an older
/// ordinal is a rollback (gxlaunch.c bootstrap_manifest), and at an EQUAL ordinal a lower serial is a rollback
/// of a relay-only re-sign. Equal everything is not a rollback (a re-fetch of the same document).
///
/// THE HORIZON (Codex relaylist-v2-r2 P1, r3 P2). A serial further than SERIAL_HORIZON_S past the moment it is judged
/// on counts as no serial at all: a cached far-future serial a stolen key signed cannot block every relay-only
/// recovery at that ordinal, and a far-future newcomer cannot win by its serial. The HELD document is judged on the
/// time it was ACCEPTED (`held_at`, its cache file's write time), never on today's clock - judged on today's, a clock
/// wound back a week made the held serial "too far ahead" and an older same-ordinal list replaced it (r3). The
/// newcomer is judged on max(now, held_at). Only this tie-break reads a clock - the update half (rank, ordinal)
/// never does, so a machine with a wrong clock still updates.
pub fn supersedes(new: &Manifest, held: &Manifest, now: u64, held_at: u64) -> bool {
    if new.rank != held.rank {
        return new.rank > held.rank;
    }
    if new.ordinal != held.ordinal {
        return new.ordinal > held.ordinal;
    }
    let usable = |m: &Manifest, at: u64| m.serial.filter(|s| *s <= at.saturating_add(SERIAL_HORIZON_S));
    match (usable(new, now.max(held_at)), usable(held, held_at)) {
        (Some(n), Some(h)) => n >= h,
        (None, Some(_)) => false,
        _ => true,
    }
}

/// base64url of the FIRST 16 BYTES of SHA-256, unpadded: 22 characters.
///
/// Truncated on purpose - the manifest has to fit one datagram on the legacy relay path, and 16
/// bytes of a strong digest is far more collision resistance than an update payload needs. Do NOT
/// "fix" this to a full digest: it would be 43 characters, never match, and read as every
/// download being corrupt.
///
/// The signer is scripts/tooling/gx-manifest.py, where this is `SHA256_KEEP = 16`.
pub fn artifact_hash(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    URL_SAFE_NO_PAD.encode(&digest[..SHA256_KEEP])
}

/// Named after the signer's own constant so the two ends of the contract are greppable as one
/// thing. Both `artifact_hash` and `ArtifactHasher` read it, and the tests below pin them to the
/// same answer - two truncation sites that disagreed would report every download as corrupt.
const SHA256_KEEP: usize = 16;

/// Constant-time-ish comparison is not required here: the expected value is public and the
/// attacker already has it. Correctness is what matters.
pub fn hash_matches(bytes: &[u8], expected: &str) -> bool {
    artifact_hash(bytes) == expected
}

/// Incremental form of [`artifact_hash`], for artifacts too large to hold in memory.
///
/// `artifact_hash` takes a slice, so a caller has to have the whole artifact resident before it
/// can learn anything about it. These are not small objects: the largest row in the live manifest
/// quoted in this file's tests is 63,948,459 bytes, and the Linux launcher payload is larger
/// still at roughly 78 MB against roughly 7 MB on macOS. Forcing a caller to choose between that
/// allocation and skipping verification is the wrong choice to offer - the branch that gets taken
/// under memory pressure is the one that skips the check, and memory pressure is exactly when a
/// download is most likely to have been truncated in the first place. Feeding a file through in
/// fixed-size reads costs one buffer regardless of artifact size.
///
/// Pure: it never touches the filesystem - the caller feeds it bytes.
pub struct ArtifactHasher {
    digest: Sha256,
}

impl ArtifactHasher {
    pub fn new() -> Self {
        Self { digest: Sha256::new() }
    }

    /// Chunk boundaries carry no meaning. A caller may split its reads wherever they land, may
    /// pass an empty slice, and need not know the total length in advance.
    pub fn update(&mut self, chunk: &[u8]) {
        self.digest.update(chunk);
    }

    /// The same 22-character string [`artifact_hash`] would have returned for the
    /// concatenation of every chunk.
    ///
    /// Takes `self` by value: SHA-256 finalisation consumes the state, and a hasher that
    /// silently kept returning its first answer after more `update` calls would be a quiet way
    /// to verify only the front of a file.
    pub fn finish(self) -> String {
        URL_SAFE_NO_PAD.encode(&self.digest.finalize()[..SHA256_KEEP])
    }
}

impl Default for ArtifactHasher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    fn fx(wire: &str) -> Manifest {
        parse(&crate::verify::verify_with(&crate::verify::tests::test_keys(), wire).expect("verifies")).expect("parses")
    }

    #[test]
    fn row4_a_ten_field_document_parses_with_one_relay_and_its_serial() {
        // S1-3 row 4: today's parser shape takes the extended manifest - relays = 1, fields 7-9 ignored but
        // the serial read. A parser that refused a trailing field would fail here, and must not.
        let m = fx(crate::fixtures::K1_HOME);
        assert_eq!(m.relays.len(), 1, "field 8's standby is NOT a relay to this parser");
        assert_eq!(m.relays[0].host, "relay1.gx.test");
        assert_eq!(m.serial, Some(1_800_000_000));
        assert_eq!(m.rank, 1);
        assert_eq!(fx(crate::fixtures::K1_EXTRA_FIELD).serial, Some(1_800_000_064), "an 11th field is ignored");
        assert_eq!(fx(crate::fixtures::K1_LEGACY7).serial, None, "a pre-S1-3 document has no serial");
        assert_eq!(fx(crate::fixtures::K1_PORT).relays[0].port, Some(9086));
        assert_eq!(fx(crate::fixtures::K2_HOME).rank, 2);
    }

    const NOW: u64 = 1_800_000_000; // S0 of the shared fixtures

    #[test]
    fn the_serial_horizon_keeps_a_far_future_serial_from_blocking_or_winning() {
        let held_future = fx(crate::fixtures::K1_FUTURE);
        let honest = fx(crate::fixtures::K1_RESIGN_HIGHER);
        assert_eq!(held_future.ordinal, honest.ordinal);
        assert!(supersedes(&honest, &held_future, NOW, NOW), "a cached year-ahead serial does not block a relay-only re-sign");
        assert!(!supersedes(&held_future, &fx(crate::fixtures::K1_HOME), NOW, NOW), "a year-ahead newcomer does not win by serial");
        assert!(!supersedes(&honest, &held_future, NOW + 365 * 86400, NOW + 365 * 86400),
            "arm: accepted when it was inside the horizon, its serial counts");
        // Codex r3 P2: a clock wound back after acceptance must not make the held serial unusable.
        let held = fx(crate::fixtures::K1_RESIGN_HIGHER);
        let older = fx(crate::fixtures::K1_HOME);
        assert!(!supersedes(&older, &held, NOW - 8 * 86400, NOW), "8 days back, an older same-ordinal list still loses");
        assert!(supersedes(&older, &held, NOW - 8 * 86400, NOW - 8 * 86400), "arm: judged on the wound-back clock, it would win");
    }

    #[test]
    fn row5_supersedes_is_rank_first_then_ordinal_then_serial() {
        let k1 = fx(crate::fixtures::K1_HOME);
        let k2 = fx(crate::fixtures::K2_HOME);
        assert!(supersedes(&k2, &k1, NOW, NOW), "a K2 document replaces a K1 one");
        assert!(!supersedes(&fx(crate::fixtures::K1_AFTER_K2), &k2, NOW, NOW), "a newer-serial K1 never replaces a K2");
        let rec = fx(crate::fixtures::K2_RECOVERY_OLDORD);
        assert!(rec.ordinal < k1.ordinal && supersedes(&rec, &k1, NOW, NOW),
            "a K2 recovery from an OLDER release ordinal still replaces a K1 document (per-rank floor)");
        assert!(!supersedes(&fx(crate::fixtures::K1_RESIGN_LOWER), &k1, NOW, NOW), "same ordinal, lower serial: a rollback");
        assert!(supersedes(&fx(crate::fixtures::K1_RESIGN_HIGHER), &k1, NOW, NOW), "same ordinal, higher serial: a relay re-sign");
        assert!(supersedes(&k1, &k1, NOW, NOW), "the same document again is not a rollback");
        assert!(supersedes(&fx(crate::fixtures::K1_OLDER), &k1, NOW, NOW),
            "a higher ORDINAL is newer for the update half (the relay list's own floor is the engine's)");
        let legacy = fx(crate::fixtures::K1_LEGACY7);
        let lower = fx(crate::fixtures::K1_RESIGN_LOWER);
        assert_eq!(legacy.ordinal, lower.ordinal);
        assert!(!supersedes(&legacy, &lower, NOW, NOW), "at an equal ordinal a serial-less document does not replace one with a serial");
    }

    use super::*;
    use crate::verify::verify;

    const LIVE: &str = "2|1787151908|0|1kuBzZqh|relay.wh1ter0se.dev,,eu|dl.wh1ter0se.dev/|win32,GeneralsX-win64-d64b81cd9.zip,WXnyJwUE3cqiaE-WAATNfg,4492409;win32,gxrt_66dd5434.zip,Tk7OXg2W6aSEcfFmLGPGeg,63948459;any,gxreq_58c3de93.zip,QP8skZClYF7Lx2vsIraAwA,29204191;win32,gxopt_3ae6628a.zip,FVVs51YS3vq6i5PuqOctXw,21109602|ecrTymzgRPT2fmz-vNDjdQhUaE98w_6Pb5JpvBDGiQ_TsJd8Rleo0267tgrYyEnLcoz4RX5ON3YxEMD5ZfapDQ";

    #[test]
    fn parses_the_live_document() {
        let m = parse(&verify(LIVE).unwrap()).unwrap();
        assert_eq!(m.version, 2);
        assert_eq!(m.ordinal, 1787151908);
        assert_eq!(m.minimum, 0);
        assert_eq!(m.sha, "1kuBzZqh");
        assert_eq!(m.base, "dl.wh1ter0se.dev/");
        assert_eq!(m.relays.len(), 1);
        assert_eq!(m.relays[0].host, "relay.wh1ter0se.dev");
        assert_eq!(m.relays[0].port, None, "an empty port field means the default");
        assert_eq!(m.relays[0].region, "eu");
        assert_eq!(m.artifacts.len(), 4);
        assert_eq!(m.artifacts[0].platform, "win32");
        assert_eq!(m.artifacts[0].size, 4492409);
    }

    #[test]
    fn a_version_this_build_does_not_speak_is_refused() {
        // Version is checked AFTER verification, so this fixture is unsigned and reaches parse
        // through the test-only constructor.
        let v = Verified::for_test("3|1|0|x|h,,eu|b/|win32,n,hh,1");
        assert_eq!(parse(&v), Err(GxError::UnsupportedVersion(3)));
    }

    #[test]
    fn fields_beyond_the_seventh_are_ignored_not_refused() {
        // This is how the format grows without breaking a client that has not been rebuilt.
        let v = Verified::for_test("2|1|0|x|h,,eu|b/|win32,n,hh,1|somethingnew|andanother");
        let m = parse(&v).unwrap();
        assert_eq!(m.artifacts.len(), 1);
    }

    #[test]
    fn an_artifact_without_exactly_four_parts_is_skipped_not_fatal() {
        let v = Verified::for_test("2|1|0|x|h,,eu|b/|win32,n,hh,1;broken,row;macos,m,hh,2");
        let m = parse(&v).unwrap();
        assert_eq!(m.artifacts.len(), 2, "the malformed row is dropped, the good ones survive");
        assert_eq!(m.artifacts[1].platform, "macos");
    }

    #[test]
    fn an_explicit_relay_port_is_honoured() {
        // The oracle (loader-tests, "an explicit port is honoured") covers this and the plan did
        // not. Without it the only relay test is the EMPTY-port case, so nothing would notice if
        // a real port were dropped on the floor.
        let v = Verified::for_test("2|1|0|x|one.example,8086,eu;two.example,,us|b/|win32,n,hh,1");
        let m = parse(&v).unwrap();
        assert_eq!(m.relays.len(), 2);
        assert_eq!(m.relays[0].port, Some(8086), "an explicit port survives");
        assert_eq!(m.relays[1].port, None, "an empty port still means the default");
        assert_eq!(m.relays[1].region, "us", "the region rides with the relay");
    }

    #[test]
    fn the_relay_list_is_capped() {
        // MAX_RELAYS had no test at all, which makes it a constant nobody has seen enforced.
        let rows: Vec<String> = (0..40).map(|i| format!("r{i}.example,,eu")).collect();
        let payload = format!("2|1|0|x|{}|b/|win32,n,hh,1", rows.join(";"));
        let v = Verified::for_test(&payload);
        assert_eq!(parse(&v).unwrap().relays.len(), MAX_RELAYS);
    }

    #[test]
    fn the_hash_is_the_first_sixteen_bytes_of_sha256_in_base64url() {
        let h = artifact_hash(b"hello world");
        assert_eq!(h.len(), 22, "22 characters, not a full 43-character digest");
        // sha256("hello world") = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
        // first 16 bytes = b94d27b9934d3e08a52e52d7da7dabfa -> base64url, unpadded
        assert_eq!(h, "uU0nuZNNPgilLlLX2n2r-g");
    }

    #[test]
    fn a_matching_hash_is_accepted_and_a_wrong_one_is_not() {
        assert!(hash_matches(b"hello world", "uU0nuZNNPgilLlLX2n2r-g"));
        assert!(!hash_matches(b"hello world!", "uU0nuZNNPgilLlLX2n2r-g"));
    }

    /// A deterministic pseudo-random body, not a repeating pattern. A run of identical bytes
    /// hashes the same whether chunks were dropped, duplicated or reordered, so it would pass
    /// every test below while proving none of them.
    fn deterministic_bytes(len: usize) -> Vec<u8> {
        let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
        (0..len)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                (state & 0xff) as u8
            })
            .collect()
    }

    #[test]
    fn one_whole_chunk_hashes_exactly_as_the_slice_form_does() {
        let input = deterministic_bytes(64 * 1024);
        let mut h = ArtifactHasher::new();
        h.update(&input);
        assert_eq!(h.finish(), artifact_hash(&input));
    }

    #[test]
    fn where_the_chunk_boundaries_fall_does_not_change_the_hash() {
        // Over 200 KB so the odd strides below produce thousands of boundaries rather than a
        // handful, and a length that is a multiple of none of them so the short final chunk is
        // always exercised too.
        let input = deterministic_bytes(200 * 1024 + 37);
        let expected = artifact_hash(&input);

        for stride in [1usize, 7, 999, 4096] {
            let mut h = ArtifactHasher::new();
            for part in input.chunks(stride) {
                h.update(part);
            }
            assert_eq!(h.finish(), expected, "stride of {stride} bytes");
        }

        // A single split landing mid-way, which is the shape a resumed or two-part read has.
        let mid = input.len() / 2 + 13;
        let mut h = ArtifactHasher::default();
        h.update(&input[..mid]);
        h.update(&input[mid..]);
        assert_eq!(h.finish(), expected, "an uneven two-part split");

        // An empty chunk is what a zero-length read looks like, and a reader that loops until it
        // sees one will hand it over. It must be a no-op, not a truncation.
        let mut h = ArtifactHasher::new();
        h.update(&input[..mid]);
        h.update(&[]);
        h.update(&input[mid..]);
        assert_eq!(h.finish(), expected, "an empty chunk mid-stream changes nothing");
    }

    #[test]
    fn a_stream_that_was_never_fed_hashes_like_an_empty_slice() {
        assert_eq!(ArtifactHasher::new().finish(), artifact_hash(&[]));
        assert_eq!(ArtifactHasher::default().finish().len(), 22);
    }

    #[test]
    fn the_streaming_form_reproduces_the_pinned_sha256_vector() {
        // Pinned against a digest computed outside this crate, so the two hashing paths agreeing
        // with each other cannot hide both being wrong. Same vector as
        // the_hash_is_the_first_sixteen_bytes_of_sha256_in_base64url, fed one byte at a time.
        //
        // The live manifest's own artifact hashes cannot be checked here: reproducing
        // WXnyJwUE3cqiaE-WAATNfg needs the 4,492,409 bytes of GeneralsX-win64-d64b81cd9.zip, and
        // gxcore may not read a file. That check belongs to whichever gxio test can open the
        // downloaded artifact.
        let mut h = ArtifactHasher::new();
        for b in b"hello world" {
            h.update(&[*b]);
        }
        assert_eq!(h.finish(), "uU0nuZNNPgilLlLX2n2r-g");
    }

    #[test]
    fn the_artifact_list_is_capped() {
        let rows: Vec<String> = (0..40).map(|i| format!("win32,n{i},hh,1")).collect();
        // Bound to a local rather than passed inline: `Verified` BORROWS its payload, so a
        // temporary would be dropped at the end of the statement.
        let payload = format!("2|1|0|x|h,,eu|b/|{}", rows.join(";"));
        let v = Verified::for_test(&payload);
        let m = parse(&v).unwrap();
        assert_eq!(m.artifacts.len(), MAX_ARTIFACTS);
    }
}
