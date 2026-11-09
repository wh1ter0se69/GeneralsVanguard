//! What the ENGINE BINARY says about itself.
//!
//! The third answer to "what is installed", and the only one that survives a development tree.
//! `BUILD.txt` is written by the packager and `gxinstalled.txt` by the updater, so a copy that no
//! packager and no updater ever touched has neither - and that is precisely the macOS case the
//! launcher used to print four dashes for.
//!
//! IT IS NOT A FOURTH PLACE TO WRITE A BUILD NUMBER. `resources/gitinfo/gitinfo.cpp.in` already
//! compiles `GitSHA1`, `GitShortSHA1`, `GitCommitDate` and `GitCommitAuthorName` into every engine
//! on every platform, release or not, and `resources/CMakeLists.txt` builds that file into the
//! engine target. This module reads what is ALREADY THERE. Nothing here asks the build system for
//! anything new, which is why it works on a binary that was built months ago.
//!
//! MEASURED 2026-09-18, on the owner's own MacBook Pro, against
//! `~/Applications/GeneralsXZH.app/Contents/Resources/bin/GeneralsXZH`:
//!
//! ```text
//! 3fd38c2daf2a77e1c9301dcd53e0d49613f2d397\0    GitSHA1
//! 3fd38c2da\0                                   GitShortSHA1
//! 2026-09-15 20:48:29 +0300\0                   GitCommitDate
//! wh1ter0se69\0                                 GitCommitAuthorName
//! ```
//!
//! and that sha resolves in this repository to "fix(engine): stop macOS minimising the game on
//! focus loss, which was breaking alt-tab", committed at exactly 2026-09-15T20:48:29+03:00. The
//! same three-token shape was confirmed on the sibling `GeneralsXZH.pre-b0b9b5bc8` (`8ff29efa7`),
//! which is the build the published Windows release carries - so this is the same identity the
//! release pipeline already prints, not a parallel one.
//!
//! IT FAILS INTO EMPTINESS, NEVER INTO A GUESS, which is the rule the whole release tier follows.
//! Every function here returns `Option`, a `None` puts the launcher back on exactly the words it
//! printed before this module existed, and no branch invents a commit from a filename, an mtime or
//! a version string.

use std::path::{Path, PathBuf};

/// What the engine compiled into itself.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct EngineStamp {
    /// `GitSHA1` - the full forty characters.
    pub commit: String,
    /// `GitShortSHA1`, exactly as the engine itself prints it. Kept rather than re-derived, so the
    /// launcher and the engine cannot disagree about how many characters "short" is.
    pub short: String,
    /// `GitCommitDate` normalised to RFC 3339, or empty when it did not have the expected shape.
    /// The SCREEN formats it - the webview has the player's locale and timezone and this process
    /// does not, which is the same split `installed_ordinal` already uses.
    pub built: String,
}

/// The biggest file this will read. The engine is ~16 MB; the ceiling exists so a mistaken path at
/// a disk image or a core dump cannot pull it into memory.
const MAX_ENGINE_BYTES: u64 = 256 * 1024 * 1024;

/// The most zero bytes that may separate two gitinfo strings - each one's own terminator included -
/// and still be read as alignment PADDING rather than as the gap to some unrelated global.
///
/// `GitSHA1`, `GitShortSHA1` and `GitCommitDate` are three separate globals, so nothing makes them
/// adjacent: the x86_64 ABI aligns an array of 16 bytes or more to 16 (and a compiler may align a
/// larger one further), which puts 13 zero bytes between the short form and the date. 64 covers
/// those alignments, and it is the same 64-byte reach the Windows packager already allows between
/// the two sha forms (`package-windows-share.ps1`).
const MAX_PADDING: usize = 64;

/// Is this token exactly a forty-character lowercase hex sha?
fn is_sha40(t: &[u8]) -> bool {
    t.len() == 40 && t.iter().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(b))
}

/// Is `short` a plausible `GitShortSHA1` FOR `full` - a strict prefix of it, of a git-like length?
///
/// THIS IS THE WHOLE DISAMBIGUATOR, and it is why a bare "find a 40-hex string" rule was not used.
/// The engine binary also contains `0000111122223333444455556666777788889999`, which is forty hex
/// characters and is not a commit. Requiring an ADJACENT token to be a prefix of this one is a
/// coincidence no decoy has produced: measured over both binaries on this machine, exactly one
/// token pair satisfies it, and it is the real one.
fn is_short_of(short: &[u8], full: &[u8]) -> bool {
    (7..=12).contains(&short.len()) && full.starts_with(short)
}

/// `2026-09-15 20:48:29 +0300` -> `2026-09-15T20:48:29+03:00`.
///
/// RFC 3339 rather than the raw form, because the raw form is what `git log --date=iso` prints and
/// `new Date()` does not parse it portably. Anything that is not exactly this shape yields `None`
/// and the field stays empty - a build with no readable date still reports its commit.
fn rfc3339_from_git_iso(t: &[u8]) -> Option<String> {
    // YYYY-MM-DD HH:MM:SS +ZZZZ
    if t.len() != 25 {
        return None;
    }
    let s = std::str::from_utf8(t).ok()?;
    let b = s.as_bytes();
    let digit = |i: usize| b[i].is_ascii_digit();
    let ok = (0..4).all(digit)
        && b[4] == b'-'
        && digit(5) && digit(6)
        && b[7] == b'-'
        && digit(8) && digit(9)
        && b[10] == b' '
        && digit(11) && digit(12)
        && b[13] == b':'
        && digit(14) && digit(15)
        && b[16] == b':'
        && digit(17) && digit(18)
        && b[19] == b' '
        && (b[20] == b'+' || b[20] == b'-')
        && (21..25).all(digit);
    if !ok {
        return None;
    }
    Some(format!("{}T{}{}{}:{}", &s[..10], &s[11..19], &s[20..21], &s[21..23], &s[23..25]))
}

/// Find the gitinfo block in an engine image.
///
/// STREAMED OVER NUL-DELIMITED TOKENS, in one pass. The engine is 16 MB and a `Vec` of every token
/// in it would cost more than the file; this allocates nothing until it has actually found the
/// block.
///
/// The order is `sha, short, date` - the order `gitinfo.cpp.in` declares them in, and what the Mac,
/// iPad and Windows binaries carry - OR ITS REVERSE, `date, short, sha`, which is what GCC emits
/// (measured 18/09/2026 on two Linux engines; the forward-only rule read nothing from either). Only
/// the two sha forms are REQUIRED: a toolchain that reorders or drops the date still yields the
/// commit, and the date field simply stays empty rather than the whole stamp being thrown away.
///
/// ZERO BYTES BETWEEN THEM ARE PADDING, UP TO [`MAX_PADDING`]. This used to demand three ADJACENT
/// tokens, which holds on the arm64 Mac and iPad binaries (measured 18/09/2026: no gap anywhere)
/// and not on a layout that aligns the 26-byte date to 16 - there the token after the short form is
/// an empty one between padding bytes, so the date came out empty, and padding between the two sha
/// forms lost the commit as well. The Linux engines measured since carry exactly such padding (13,
/// 7 and 21 zero bytes between author, date, short and sha). A run of zeros is the only thing
/// skipped: any other token in between still ends the match, exactly as before.
pub fn parse_engine_stamp(bytes: &[u8]) -> Option<EngineStamp> {
    let stamp = |sha: &[u8], short: &[u8], date: &[u8]| EngineStamp {
        commit: String::from_utf8_lossy(sha).into_owned(),
        short: String::from_utf8_lossy(short).into_owned(),
        built: rfc3339_from_git_iso(date).unwrap_or_default(),
    };

    // The candidate so far, and how many EMPTY tokens have passed since its last token. A run of k
    // zero bytes after a token's own terminator splits into k empty tokens, so the whole gap is
    // k + 1 zero bytes, and it stays padding while that is at most MAX_PADDING.
    let mut sha: Option<&[u8]> = None;
    let mut short: Option<&[u8]> = None;
    let mut gap = 0usize;
    // THE REVERSED ORDER TOO - `date, short, sha` - which is how GCC lays the globals out:
    // measured 18/09/2026 on the 093922015 and 85f9a6650 Linux engines, where the forward rule
    // found nothing. The last two tokens, each kept only while nothing but padding follows it.
    let mut last: Option<&[u8]> = None;
    let mut before_last: Option<&[u8]> = None;

    for tok in bytes.split(|&b| b == 0) {
        if tok.is_empty() {
            gap += 1;
            if gap + 1 > MAX_PADDING {
                // Too far for padding. A sha with its short form is still a commit, with no date;
                // a sha alone is nothing.
                if let (Some(s), Some(sh)) = (sha, short) {
                    return Some(stamp(s, sh, &[]));
                }
                sha = None;
                last = None;
            }
            continue;
        }
        if is_sha40(tok) {
            if let Some(sh) = last.filter(|sh| is_short_of(sh, tok)) {
                return Some(stamp(tok, sh, before_last.unwrap_or_default()));
            }
        }
        before_last = last;
        last = Some(tok);
        match (sha, short) {
            (Some(s), Some(sh)) => return Some(stamp(s, sh, tok)),
            (Some(s), None) if is_short_of(tok, s) => {
                short = Some(tok);
                gap = 0;
                continue;
            }
            _ => {}
        }
        sha = is_sha40(tok).then_some(tok);
        short = None;
        gap = 0;
    }
    // The block was the last thing in the image.
    match (sha, short) {
        (Some(s), Some(sh)) => Some(stamp(s, sh, &[])),
        _ => None,
    }
}

/// Read one file and look for the block. `None` for anything unreadable, oversized, or without it.
///
/// A shell script is the case worth naming: on macOS `Contents/MacOS/GeneralsXZH` is a byte-for-byte
/// copy of `run.sh` (measured - identical md5), so the obvious candidate is not the binary at all.
/// It simply yields `None` here and the next candidate is tried.
pub fn read_engine_stamp(path: &Path) -> Option<EngineStamp> {
    let meta = std::fs::metadata(path).ok()?;
    if !meta.is_file() || meta.len() > MAX_ENGINE_BYTES {
        return None;
    }
    let bytes = std::fs::read(path).ok()?;
    parse_engine_stamp(&bytes)
}

/// Where the engine executable sits, given the directory discovery found.
///
/// `dir` is whatever `game::discover()` called the install, which on macOS is the `.app` BUNDLE and
/// on the deploy layouts is a plain folder. The bundle case is listed first and explicitly:
/// `bundle-macos-zh.sh` puts the Mach-O at `Contents/Resources/bin/`, and `Contents/MacOS/` holds
/// the wrapper script that execs it.
pub fn engine_binary_candidates(dir: &Path) -> Vec<PathBuf> {
    let mut v = Vec::new();
    if dir.extension().and_then(|e| e.to_str()) == Some("app") {
        let c = dir.join("Contents");
        v.push(c.join("Resources").join("bin").join("GeneralsXZH"));
        v.push(c.join("MacOS").join("GeneralsXZH"));
    }
    // The deploy and Windows layouts, and a bundle whose shape this build does not know.
    v.push(dir.join("GeneralsXZH"));
    v.push(dir.join("generalszh.exe"));
    v
}

/// The stamp of the engine this launcher would actually start, and the file it was read from.
///
/// ANCHORED ON `game::discover()`, NOT ON A SECOND SEARCH. Discovery is what PLAY runs and what the
/// About page already names under "Installed to"; a private search here could name a different
/// build from the one the player is about to launch, which is the exact failure the whole module is
/// meant to remove.
pub fn find_engine_stamp() -> Option<(PathBuf, EngineStamp)> {
    engine_stamp_in(crate::game::discover().install_dir.as_ref()?)
}

/// The engine binary inside `dir` and its stamp - the first candidate that carries one. The
/// launcher's installer hashes THIS file into the bundle's stamp, so the reader and the writer
/// cannot pick two different files.
pub fn engine_stamp_in(dir: &Path) -> Option<(PathBuf, EngineStamp)> {
    engine_binary_candidates(dir)
        .into_iter()
        .find_map(|p| read_engine_stamp(&p).map(|s| (p, s)))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a fake image: junk, the block, junk.
    fn image(tokens: &[&str]) -> Vec<u8> {
        let mut v = b"\x7fELF some junk\0/usr/lib/libc\0".to_vec();
        for t in tokens {
            v.extend_from_slice(t.as_bytes());
            v.push(0);
        }
        v.extend_from_slice(b"trailing junk\0");
        v
    }

    const SHA: &str = "3fd38c2daf2a77e1c9301dcd53e0d49613f2d397";

    #[test]
    fn the_real_block_is_read_whole() {
        let img = image(&[SHA, "3fd38c2da", "2026-09-15 20:48:29 +0300", "wh1ter0se69"]);
        let s = parse_engine_stamp(&img).expect("a stamp");
        assert_eq!(s.commit, SHA);
        assert_eq!(s.short, "3fd38c2da");
        assert_eq!(s.built, "2026-09-15T20:48:29+03:00");
    }

    /// The decoy that is really in the engine: forty hex characters, not a commit. It is rejected
    /// because nothing that looks like its own short form follows it.
    #[test]
    fn a_forty_hex_string_that_is_not_a_commit_is_refused() {
        let img = image(&["0000111122223333444455556666777788889999", "not a sha", "x"]);
        assert_eq!(parse_engine_stamp(&img), None);
    }

    /// A build whose date field is missing or reshaped still reports its commit.
    #[test]
    fn the_commit_survives_a_date_this_build_cannot_read() {
        let img = image(&[SHA, "3fd38c2da", "Mon Sep 15 20:48:29 2026 +0300"]);
        let s = parse_engine_stamp(&img).expect("a stamp");
        assert_eq!(s.short, "3fd38c2da");
        assert_eq!(s.built, "", "an unreadable date must not become a wrong one");
    }

    #[test]
    fn a_file_with_no_block_yields_nothing() {
        assert_eq!(parse_engine_stamp(b"#!/bin/bash\nexec ./GeneralsXZH\n"), None);
    }

    /// Uppercase is not what `gitinfo.cpp.in` writes, and accepting it would widen the decoy
    /// surface for nothing.
    #[test]
    fn an_uppercase_sha_is_not_matched() {
        let up = SHA.to_ascii_uppercase();
        let img = image(&[&up, &up[..9]]);
        assert_eq!(parse_engine_stamp(&img), None);
    }

    /// The short token has to belong to the long one. A neighbouring hex string that is not a
    /// prefix must not be adopted as the short form.
    #[test]
    fn a_neighbour_that_is_not_a_prefix_is_refused() {
        let img = image(&[SHA, "abcdef123", "2026-09-15 20:48:29 +0300"]);
        assert_eq!(parse_engine_stamp(&img), None);
    }

    /// A fake image laid out the way a linker lays out separate globals: each token, its own
    /// terminating NUL, then `pad` MORE zero bytes before the next one starts.
    fn padded(parts: &[(&str, usize)]) -> Vec<u8> {
        let mut v = b"\x7fELF some junk\0".to_vec();
        for (t, pad) in parts {
            v.extend_from_slice(t.as_bytes());
            v.push(0);
            v.extend(std::iter::repeat(0u8).take(*pad));
        }
        v.extend_from_slice(b"trailing junk\0");
        v
    }

    const DATE: &str = "2026-09-15 20:48:29 +0300";

    /// AN x86_64 LAYOUT IN THE FORWARD ORDER (Linux GCC's is reversed - see below). `GitSHA1` is 41
    /// bytes and `GitShortSHA1` 10, so the short form ends at offset 51; `GitCommitDate` is 26
    /// bytes, and an array of 16 bytes or more is aligned to 16 there, so it starts at 64 -
    /// thirteen zero bytes of padding in between.
    /// The strict three-adjacent-tokens window read that padding as an empty date.
    #[test]
    fn alignment_padding_before_the_date_is_skipped() {
        let img = padded(&[(SHA, 0), ("3fd38c2da", 13), (DATE, 0), ("wh1ter0se69", 0)]);
        let s = parse_engine_stamp(&img).expect("a stamp");
        assert_eq!(s.commit, SHA);
        assert_eq!(s.built, "2026-09-15T20:48:29+03:00", "the date must survive alignment padding");
    }

    /// Padding can land between the two sha forms as well (`package-windows-share.ps1` records the
    /// short form sitting "a few bytes after" the full one in a real `generalszh.exe`).
    #[test]
    fn alignment_padding_before_the_short_form_is_skipped() {
        let img = padded(&[(SHA, 7), ("3fd38c2da", 13), (DATE, 0)]);
        let s = parse_engine_stamp(&img).expect("a stamp across padding between the sha forms");
        assert_eq!(s.short, "3fd38c2da");
        assert_eq!(s.built, "2026-09-15T20:48:29+03:00");
    }

    /// THE LINUX GCC LAYOUT, as measured 18/09/2026 on the 093922015 and 85f9a6650 Linux engines:
    /// the globals come out in REVERSE declaration order - author, date, short, sha - with 13, 7
    /// and 21 zero bytes between them. The sha-first rule read no commit from any Linux engine.
    #[test]
    fn the_gcc_reversed_layout_yields_its_commit_and_date() {
        let img = padded(&[("wh1ter0se69", 12), (DATE, 6), ("3fd38c2daf2", 20), (SHA, 3)]);
        let s = parse_engine_stamp(&img).expect("a stamp from the GCC layout");
        assert_eq!(s.commit, SHA);
        assert_eq!(s.short, "3fd38c2daf2");
        assert_eq!(s.built, "2026-09-15T20:48:29+03:00");

        // The same bounds as the forward order: a short form beyond the padding is not adopted,
        // and a date beyond it is not this build's.
        let img = padded(&[("3fd38c2daf2", MAX_PADDING), (SHA, 0)]);
        assert_eq!(parse_engine_stamp(&img), None, "a short form beyond the bound is not adopted");
        let img = padded(&[(DATE, MAX_PADDING), ("3fd38c2daf2", 0), (SHA, 0)]);
        assert_eq!(parse_engine_stamp(&img).expect("the commit is kept").built, "");
        let img = padded(&[("abcdef123", 0), (SHA, 0)]);
        assert_eq!(parse_engine_stamp(&img), None, "a neighbour that is not its prefix is refused");
    }

    /// PADDING, NOT DISTANCE. Zero bytes are skipped only up to [`MAX_PADDING`]; past that the next
    /// token is some other global and is not adopted. Still safe in both directions: the commit
    /// survives a date it will not reach, and a lone sha with no short form near it is refused.
    #[test]
    fn padding_is_skipped_only_up_to_a_bound() {
        let img = padded(&[(SHA, 0), ("3fd38c2da", MAX_PADDING), (DATE, 0)]);
        let s = parse_engine_stamp(&img).expect("the commit is kept");
        assert_eq!(s.built, "", "a date beyond the padding bound is not this build's date");

        let img = padded(&[(SHA, MAX_PADDING), ("3fd38c2da", 0), (DATE, 0)]);
        assert_eq!(parse_engine_stamp(&img), None, "a short form beyond the bound is not adopted");

        // The largest gap that IS padding: MAX_PADDING zero bytes in all, terminator included.
        let img = padded(&[(SHA, 0), ("3fd38c2da", MAX_PADDING - 1), (DATE, 0)]);
        assert_eq!(parse_engine_stamp(&img).expect("a stamp").built, "2026-09-15T20:48:29+03:00");
    }

    /// A block at the very end of the image - the short form's own terminator and then nothing -
    /// still names its commit. The three-token window answered this through the empty token after
    /// that last NUL; the padding-aware scan has to answer it at end of input instead.
    #[test]
    fn a_block_at_the_end_of_the_image_still_yields_its_commit() {
        let mut img = b"junk\0".to_vec();
        img.extend_from_slice(SHA.as_bytes());
        img.push(0);
        img.extend_from_slice(b"3fd38c2da\0");
        let s = parse_engine_stamp(&img).expect("a stamp");
        assert_eq!(s.short, "3fd38c2da");
        assert_eq!(s.built, "");
    }

    #[test]
    fn the_mac_bundle_binary_is_the_first_candidate() {
        let v = engine_binary_candidates(Path::new("/x/GeneralsXZH.app"));
        assert_eq!(v[0], Path::new("/x/GeneralsXZH.app/Contents/Resources/bin/GeneralsXZH"));
        assert!(
            v.iter().any(|p| p.ends_with("Contents/MacOS/GeneralsXZH")),
            "the wrapper location is still tried, in case a build puts the Mach-O there"
        );
    }

    #[test]
    fn a_plain_directory_gets_the_deploy_layout() {
        let v = engine_binary_candidates(Path::new("/x/GeneralsZH"));
        assert_eq!(v[0], Path::new("/x/GeneralsZH/GeneralsXZH"));
    }

    /// THE ONLY CHECK THAT TOUCHES A REAL ENGINE. Every test above builds its own image, which
    /// proves the RULE and not that the rule matches what a compiler actually emits - and the rule
    /// was derived from two binaries, so a test that only ever sees synthetic input could agree
    /// with a mistaken reading of them forever.
    ///
    /// Opt-in through the environment rather than hardcoding a path, because the engine is not in
    /// this repository and no CI machine has one. Point it at a built engine and it checks that a
    /// commit came out and that the short form really is the prefix the panel will print:
    ///
    /// ```text
    /// GX_ENGINE_STAMP_FILE=~/Applications/GeneralsXZH.app/Contents/Resources/bin/GeneralsXZH \
    ///   cargo test -p gxio --offline enginestamp -- --ignored --nocapture
    /// ```
    ///
    /// IGNORED, NOT SKIPPED-AS-PASSING. An early `return` when the variable is unset would count as
    /// a pass in every normal run while measuring nothing - a green number with no check behind it.
    /// So it reports as `ignored` until asked for, and asked for without its input it FAILS rather
    /// than quietly agreeing.
    #[test]
    #[ignore = "needs GX_ENGINE_STAMP_FILE pointing at a built engine"]
    fn a_real_engine_binary_yields_its_own_commit() {
        let p = std::env::var_os("GX_ENGINE_STAMP_FILE")
            .expect("set GX_ENGINE_STAMP_FILE to a built engine binary to run this check");
        let p = PathBuf::from(p);
        let s = read_engine_stamp(&p).unwrap_or_else(|| panic!("no stamp in {}", p.display()));
        eprintln!("commit={} short={} built={}", s.commit, s.short, s.built);
        assert_eq!(s.commit.len(), 40, "a full sha");
        assert!(s.commit.starts_with(&s.short), "the short form must belong to the long one");
        assert!(!s.built.is_empty(), "a real build carries a readable date");
    }

    #[test]
    fn a_bad_offset_shape_is_not_a_date() {
        assert_eq!(rfc3339_from_git_iso(b"2026-09-15 20:48:29 Z0300"), None);
        assert_eq!(rfc3339_from_git_iso(b"2026-09-15T20:48:29 +0300"), None);
        assert_eq!(rfc3339_from_git_iso(b"2026-09-15 20:48:29 +030"), None);
    }

    #[test]
    fn a_negative_offset_is_kept() {
        assert_eq!(
            rfc3339_from_git_iso(b"2026-09-15 20:48:29 -0500").as_deref(),
            Some("2026-09-15T20:48:29-05:00")
        );
    }
}
