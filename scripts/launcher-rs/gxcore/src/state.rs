use crate::manifest::MAX_ARTIFACTS;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstalledItem {
    pub platform: String,
    pub name: String,
    pub hash: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct InstallState {
    pub ordinal: u64,
    pub items: Vec<InstalledItem>,
}

impl InstallState {
    /// Never fails. A record that cannot be read means "nothing is installed", which is a state
    /// the updater can recover from; an error here would strand a player with no route forward.
    pub fn parse(text: &str) -> InstallState {
        let mut st = InstallState::default();
        for line in text.lines() {
            let line = line.trim();
            if let Some(rest) = line.strip_prefix("ordinal ") {
                st.ordinal = rest.trim().parse().unwrap_or(0);
            } else if let Some(rest) = line.strip_prefix("item ") {
                if st.items.len() == MAX_ARTIFACTS {
                    continue;
                }
                let p: Vec<&str> = rest.split_whitespace().collect();
                if p.len() != 3 {
                    continue;
                }
                st.items.push(InstalledItem {
                    platform: p[0].to_string(),
                    name: p[1].to_string(),
                    hash: p[2].to_string(),
                });
            }
        }
        st
    }

    pub fn render(&self) -> String {
        let mut s = format!("ordinal {}\n", self.ordinal);
        for it in &self.items {
            s.push_str(&format!("item {} {} {}\n", it.platform, it.name, it.hash));
        }
        s
    }

    /// Platform AND name. Two platforms may ship a file with the same name, so neither alone
    /// identifies an artifact. gxlaunch.c:1718-1727 (`state_hash_of`).
    ///
    /// The platform comparison is case-INSENSITIVE, which is a deliberate divergence from that C
    /// - see `the_platform_is_matched_case_insensitively_unlike_the_c` below for why, and why it
    /// cannot accept wrong bytes.
    pub fn has(&self, platform: &str, name: &str) -> Option<&InstalledItem> {
        self.items
            .iter()
            .find(|i| i.platform.eq_ignore_ascii_case(platform) && i.name == name)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let s = InstallState {
            ordinal: 1787151908,
            items: vec![
                InstalledItem { platform: "win32".into(), name: "a.zip".into(), hash: "H1".into() },
                InstalledItem { platform: "macos".into(), name: "a.zip".into(), hash: "H2".into() },
            ],
        };
        let parsed = InstallState::parse(&s.render());
        assert_eq!(parsed.ordinal, s.ordinal);
        assert_eq!(parsed.items.len(), 2);
    }

    #[test]
    fn matches_on_platform_and_name_together() {
        // Two platforms shipping the same filename is the case this exists for.
        let s = InstallState::parse("ordinal 5\nitem win32 a.zip H1\nitem macos a.zip H2\n");
        assert_eq!(s.has("win32", "a.zip").unwrap().hash, "H1");
        assert_eq!(s.has("macos", "a.zip").unwrap().hash, "H2");
        assert!(s.has("linux", "a.zip").is_none());
    }

    #[test]
    fn an_unreadable_record_is_an_empty_one_not_an_error() {
        // A corrupt or absent record must mean "nothing is installed", which is recoverable.
        // Returning an error here would strand a player with no route forward.
        let s = InstallState::parse("garbage\n\n???");
        assert_eq!(s.ordinal, 0);
        assert!(s.items.is_empty());
    }

    #[test]
    fn item_lines_are_capped() {
        let mut text = String::from("ordinal 1\n");
        for i in 0..40 { text.push_str(&format!("item win32 n{i} H\n")); }
        assert_eq!(InstallState::parse(&text).items.len(), crate::MAX_ARTIFACTS);
    }

    #[test]
    fn the_platform_is_matched_case_insensitively_unlike_the_c() {
        // A DELIBERATE divergence from gxlaunch.c:1718-1727, which uses strcmp on both fields.
        //
        // The manifest format treats the platform token as case-insensitive - that is what the
        // _stricmp at gxlaunch.c:2060 establishes - so a lookup keyed on it must be too. Without
        // this, a manifest saying "WIN32" would be recognised as ours by the wanted-check and
        // then never found by the state lookup, and every artifact would re-download forever on
        // a publisher's case change.
        //
        // It cannot accept wrong bytes: the hash comparison still gates, so the only reachable
        // effect is avoiding a redundant download. The NAME stays case-sensitive, because a
        // filename is not a token - two files differing only in case are two files.
        let s = InstallState::parse("ordinal 5\nitem win32 a.zip H1\n");
        assert_eq!(s.has("WIN32", "a.zip").unwrap().hash, "H1", "platform token: case-insensitive");
        assert!(s.has("win32", "A.ZIP").is_none(), "filename: case-sensitive");
    }
}
