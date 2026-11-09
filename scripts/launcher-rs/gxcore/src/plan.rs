use crate::manifest::{Artifact, Manifest};
use crate::state::InstallState;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    UpToDate,
    Update(Vec<Artifact>),
    /// This document is older than what is installed. Nothing is downloaded. gxlaunch.c:2048.
    ///
    /// The C expresses this as "zero artifacts wanted", which at the call site is
    /// indistinguishable from up-to-date. Split into its own variant here because the launcher UI
    /// has to say something different. The BEHAVIOUR is identical: nothing is downloaded either
    /// way. Do not collapse it back.
    OlderThanInstalled { manifest: u64, installed: u64 },
}

/// An artifact is ours if it names our platform, or the literal "any". Case-insensitive on both,
/// matching the _stricmp at gxlaunch.c:2060.
///
/// "any" is the platform-independent artifact - the community-patch overlay is one, and it is why
/// the feature exists at all: a friend without it has a different dataID and cannot join.
fn wanted(a: &Artifact, platform: &str) -> bool {
    a.platform.eq_ignore_ascii_case(platform) || a.platform.eq_ignore_ascii_case("any")
}

/// Decide what this install needs.
///
/// `minimum` is deliberately NOT consulted. The C does not consult it here either; it is a
/// separate concern and involving it would silently change which builds are allowed to run.
pub fn decide(m: &Manifest, st: &InstallState, platform: &str) -> Verdict {
    // Strictly '<'. An equal ordinal falls through, so a re-publish can still correct a hash.
    if m.ordinal < st.ordinal {
        return Verdict::OlderThanInstalled { manifest: m.ordinal, installed: st.ordinal };
    }

    let needed: Vec<Artifact> = m
        .artifacts
        .iter()
        .filter(|a| wanted(a, platform))
        .filter(|a| match st.has(&a.platform, &a.name) {
            Some(have) => have.hash != a.hash,
            None => true,
        })
        .cloned()
        .collect();

    if needed.is_empty() {
        Verdict::UpToDate
    } else {
        Verdict::Update(needed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{Artifact, Manifest};
    use crate::state::{InstallState, InstalledItem};

    fn m(ordinal: u64, minimum: u64, arts: &[(&str, &str, &str)]) -> Manifest {
        Manifest {
            version: 2, ordinal, minimum, rank: 1, serial: None, sha: "x".into(), relays: vec![], base: "b/".into(),
            artifacts: arts.iter().map(|(p, n, h)| Artifact {
                platform: p.to_string(), name: n.to_string(), hash: h.to_string(), size: 1,
            }).collect(),
        }
    }
    fn st(ordinal: u64, items: &[(&str, &str, &str)]) -> InstallState {
        InstallState { ordinal, items: items.iter().map(|(p, n, h)| InstalledItem {
            platform: p.to_string(), name: n.to_string(), hash: h.to_string(),
        }).collect() }
    }

    #[test]
    fn nothing_to_do_when_every_wanted_artifact_matches() {
        let v = decide(&m(5, 0, &[("win32", "a.zip", "H")]), &st(5, &[("win32", "a.zip", "H")]), "win32");
        assert!(matches!(v, Verdict::UpToDate));
    }

    #[test]
    fn a_changed_hash_is_an_update_even_at_the_same_ordinal() {
        let v = decide(&m(5, 0, &[("win32", "a.zip", "NEW")]), &st(5, &[("win32", "a.zip", "OLD")]), "win32");
        match v { Verdict::Update(a) => assert_eq!(a[0].name, "a.zip"), _ => panic!("{v:?}") }
    }

    #[test]
    fn other_platforms_are_ignored_but_any_is_wanted() {
        let man = m(5, 0, &[("macos", "m.zip", "H"), ("any", "shared.zip", "H"), ("win32", "w.zip", "H")]);
        match decide(&man, &st(0, &[]), "win32") {
            Verdict::Update(a) => {
                let names: Vec<&str> = a.iter().map(|x| x.name.as_str()).collect();
                assert_eq!(names, vec!["shared.zip", "w.zip"], "macos is not ours; any is");
            }
            v => panic!("{v:?}"),
        }
    }

    #[test]
    fn a_manifest_older_than_the_install_wants_nothing() {
        // gxlaunch.c:2048. A replayed old document must not walk a player backwards.
        let v = decide(&m(3, 0, &[("win32", "a.zip", "H")]), &st(9, &[]), "win32");
        assert!(matches!(v, Verdict::OlderThanInstalled { manifest: 3, installed: 9 }));
    }

    #[test]
    fn an_equal_ordinal_still_compares_hashes() {
        // Strictly '<', not '<='. A re-publish at the same ordinal must still be able to correct
        // a bad hash, and making this '<=' would silently disable that.
        let v = decide(&m(5, 0, &[("win32", "a.zip", "NEW")]), &st(5, &[("win32", "a.zip", "OLD")]), "win32");
        assert!(matches!(v, Verdict::Update(_)));
    }

    #[test]
    fn the_platform_match_is_case_insensitive() {
        // gxlaunch.c:2060 uses _stricmp for both the platform and the "any" literal.
        let v = decide(&m(5, 0, &[("WIN32", "a.zip", "H"), ("ANY", "b.zip", "H")]), &st(0, &[]), "win32");
        match v { Verdict::Update(a) => assert_eq!(a.len(), 2), v => panic!("{v:?}") }
    }

    #[test]
    fn the_same_filename_on_another_platform_does_not_satisfy_ours() {
        let v = decide(&m(5, 0, &[("win32", "a.zip", "H")]), &st(5, &[("macos", "a.zip", "H")]), "win32");
        assert!(matches!(v, Verdict::Update(_)));
    }
}
