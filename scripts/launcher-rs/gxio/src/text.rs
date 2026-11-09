//! Reading text files that Windows tools wrote.
//!
//! WHY THIS EXISTS. Windows PowerShell's Set-Content and Out-File write a UTF-8 BYTE ORDER MARK
//! by default - the three bytes EF BB BF - and every one of this project's config files can be
//! produced or hand-edited that way. The BOM is invisible in every editor and in `Get-Content`,
//! so a file that looks perfect on screen parses with an extra character glued to the front of
//! its FIRST line and nothing else.
//!
//! Measured 2026-08-20: the launcher told a player `!!GXCameoHD.big is missing or empty. Without
//! it every join is refused, including by friends.` while the file sat in the right folder at
//! 47,589,817 bytes. `required.txt` began EF BB BF, so the name it looked for was U+FEFF followed
//! by the name. Only the first entry was affected, which is the signature to recognise: one
//! broken row at the top of a file whose other rows are fine.
//!
//! Rust's `str::lines` already handles CRLF, so line endings are not the problem here and
//! stripping `\r` separately is unnecessary.

/// Drop a leading UTF-8 BOM. Everything that reads a config file should go through this.
pub fn strip_bom(s: &str) -> &str {
    s.strip_prefix('\u{feff}').unwrap_or(s)
}

/// Read a text file, BOM removed. `None` when it cannot be read - callers decide whether an
/// absent file is normal, and for most of these it is.
pub fn read_to_string_no_bom(path: &std::path::Path) -> Option<String> {
    std::fs::read_to_string(path).ok().map(|s| strip_bom(&s).to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strips_a_utf8_bom() {
        assert_eq!(strip_bom("\u{feff}!!GXCameoHD.big"), "!!GXCameoHD.big");
    }

    #[test]
    fn leaves_a_clean_string_alone() {
        assert_eq!(strip_bom("!!GXCameoHD.big"), "!!GXCameoHD.big");
    }

    #[test]
    fn only_the_first_line_was_ever_affected() {
        // The exact shape of the live bug: row one broken, the rest fine. Reproduce it so a
        // future reader can see why the symptom looked so arbitrary.
        let raw = "\u{feff}!!GXCameoHD.big\r\n!HotkeysLeikezeIndicatorsZH.big\r\n500_900_CommunityPatch_CoreINI.big\r\n";
        let bad: Vec<&str> = raw.lines().collect();
        assert_ne!(bad[0], "!!GXCameoHD.big", "this is the bug");
        assert_eq!(bad[1], "!HotkeysLeikezeIndicatorsZH.big", "and this is why only one row failed");

        let good: Vec<&str> = strip_bom(raw).lines().collect();
        assert_eq!(good, vec![
            "!!GXCameoHD.big",
            "!HotkeysLeikezeIndicatorsZH.big",
            "500_900_CommunityPatch_CoreINI.big",
        ]);
    }
}
