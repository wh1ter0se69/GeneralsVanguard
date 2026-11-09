//! Map previews for the admin panel, uploaded by the launcher.
//!
//! GeneralsX @feature Claude 25/09/2026 The owner: "the LAUNCHER uploads previews - automatic, no
//! toggle, no consent prompt; keep previews forever". Plan:
//! docs/superpowers/plans/2026-09-25-map-previews-launcher.md.
//!
//! ONCE PER LAUNCH, IN THE BACKGROUND, AND ONLY WHAT IS ASKED FOR. The panel records every map a live
//! game is on that it has no picture for; gx-oauth's `GET /mappreview/wanted` lists them. This asks for
//! that list, intersects it with the player's own `<userdata>/Maps` folder, and uploads only those
//! `.tga` files (`PUT /mappreview/<slug>`), at most [`MAX_PER_LAUNCH`]. A player with 1,400 custom maps
//! sends nothing for the 1,399 nobody is looking at, and never sends the list of what they have.
//!
//! THE KEY IS THE PANEL'S SLUG, computed here from the map file's leaf name exactly as it reaches the
//! panel: the engine's advert cuts the leaf to 63 bytes and drops one `.map` (LANAPI.cpp, the GXADV
//! writer), the relay turns control bytes and `|` into `/` (relay.js sanitizeText), and the panel
//! takes the last path segment, drops one more `.map` and folds to `[a-z0-9-]` (lib/components.js
//! gxMapSlug). [`slug`] is that chain; `map-slug-vectors.tsv` is shared with the panel's own suite so
//! the two cannot drift.
//!
//! SIGNED IN OR NOTHING. The identity is the engine's own `<userdata>/GXLogin.txt` - the assertion the
//! Discord sign-in delivered. No file, an expired one, or no map to offer: no request is made at all.
//! The assertion is a bearer token, so it never goes on curl's command line (any local user can read
//! argv); curl reads its whole configuration from stdin (`-K -`).
//!
//! IPAD HAS NO LAUNCHER. A custom map that only an iPad has never gets a preview from that iPad; it
//! gets one the first time any desktop player who has the same map name opens their launcher.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// gx-oauth, the identity Worker. Overridable at build time for a local `wrangler dev`.
pub const BASE_URL: &str = match option_env!("GX_MAPPREVIEW_URL") {
    Some(u) => u,
    None => "https://auth.wh1ter0se.dev",
};
/// The Worker's cap (`GX_MAPPREVIEW_MAX_BYTES`, tools/oauth/mappreview.js). A bigger file is not sent.
pub const MAX_BYTES: u64 = 66_000;
/// Uploads per launch. The server allows 30 per account per day; this keeps one launch well inside it.
pub const MAX_PER_LAUNCH: usize = 20;
const LOGIN_FILE: &str = "GXLogin.txt";

/// The panel's slug for a map file's leaf name (`Foo Bar.map`), or None when nothing usable is left.
pub fn slug(leaf: &str) -> Option<String> {
    // Engine: strlcpy into char[64] (63 bytes), then drop a trailing ".map" if longer than 4.
    let b = leaf.as_bytes();
    let mut wire: Vec<u8> = b[..b.len().min(63)].to_vec();
    if wire.len() > 4 && wire[wire.len() - 4..].eq_ignore_ascii_case(b".map") {
        wire.truncate(wire.len() - 4);
    }
    // Relay: sanitizeText, on the raw bytes, before the UTF-8 decode.
    for c in wire.iter_mut() {
        if *c < 0x20 || *c == 0x7f || *c == b'|' {
            *c = b'/';
        }
    }
    let text = String::from_utf8_lossy(&wire);
    // Panel: the last path segment, one more ".map", lowercase, fold.
    let last = text.rsplit(['\\', '/']).next().unwrap_or("");
    let stem = if last.len() >= 4 && last.is_char_boundary(last.len() - 4) && last[last.len() - 4..].eq_ignore_ascii_case(".map") {
        &last[..last.len() - 4]
    } else {
        last
    };
    let mut out = String::new();
    for ch in stem.to_lowercase().chars() {
        if ch.is_ascii_lowercase() || ch.is_ascii_digit() {
            out.push(ch);
        } else if !out.ends_with('-') {
            out.push('-');
        }
    }
    let s = out.trim_matches('-');
    (is_slug(s)).then(|| s.to_string())
}

/// `^[a-z0-9]+(-[a-z0-9]+)*$`, at most 64: the only shape the Worker accepts.
pub fn is_slug(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 64
        && !s.starts_with('-')
        && !s.ends_with('-')
        && !s.contains("--")
        && s.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-')
}

/// slug -> preview file, for every `*.map` in `maps` (and one folder down) with a `.tga` beside it
/// that is small enough to send. The first file for a slug wins.
pub fn local_previews(maps: &Path) -> BTreeMap<String, PathBuf> {
    let mut out = BTreeMap::new();
    let mut dirs = vec![maps.to_path_buf()];
    if let Ok(it) = std::fs::read_dir(maps) {
        dirs.extend(it.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_dir()));
    }
    for d in dirs {
        let Ok(it) = std::fs::read_dir(&d) else { continue };
        let mut files: Vec<PathBuf> = it.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_file()).collect();
        files.sort();
        for m in files {
            let Some(name) = m.file_name().and_then(|n| n.to_str()) else { continue };
            if name.len() <= 4 || !name.get(name.len() - 4..).is_some_and(|x| x.eq_ignore_ascii_case(".map")) {
                continue;
            }
            let Some(key) = slug(name) else { continue };
            // The engine swaps the extension on the same path (MapUtil.cpp getMapPreviewImage).
            let tga = ["tga", "TGA"].iter().map(|x| m.with_extension(x)).find(|p| p.is_file());
            let Some(tga) = tga else { continue };
            if std::fs::metadata(&tga).map(|md| md.len() == 0 || md.len() > MAX_BYTES).unwrap_or(true) {
                continue;
            }
            out.entry(key).or_insert(tga);
        }
    }
    out
}

/// The first line of the login file, if it is an assertion that has not expired. Only the shape and
/// the expiry are looked at - the Worker verifies the signature; this only avoids a pointless request.
pub fn login_token(text: &str, now_s: u64) -> Option<String> {
    let t = text.lines().next()?.trim();
    if t.is_empty() || t.len() > 256 || !t.bytes().all(|c| (0x21..=0x7e).contains(&c)) {
        return None;
    }
    let f: Vec<&str> = t.split('~').collect();
    if f.len() < 6 || f[0] != "1" || f[5] == "b" {
        return None;
    }
    let exp: u64 = f[4].parse().ok()?;
    (exp + 60 >= now_s).then(|| t.to_string())
}

/// The slugs out of the Worker's `{"ok":true,"wanted":[...]}`. Strict: anything that is not a list
/// of slugs yields nothing, never a guess.
pub fn parse_wanted(body: &str) -> Vec<String> {
    let Some(start) = body.find("\"wanted\":[") else { return Vec::new() };
    let rest = &body[start + 10..];
    let Some(end) = rest.find(']') else { return Vec::new() };
    let inner = rest[..end].trim();
    if inner.is_empty() {
        return Vec::new();
    }
    let mut out = Vec::new();
    for item in inner.split(',') {
        let s = item.trim();
        let Some(s) = s.strip_prefix('"').and_then(|s| s.strip_suffix('"')) else { return Vec::new() };
        if !is_slug(s) {
            return Vec::new();
        }
        out.push(s.to_string());
    }
    out
}

/// Only https, or plain http to this machine (a local `wrangler dev`).
pub fn url_allowed(url: &str) -> bool {
    url.starts_with("https://") || url.starts_with("http://127.0.0.1:") || url.starts_with("http://localhost:")
}

/// A string for curl's config file: double-quoted, `\` and `"` escaped. None for anything with a
/// line break or control byte, which the format cannot carry safely. Also `crashlogs`' upload.
pub(crate) fn quoted(s: &str) -> Option<String> {
    if s.chars().any(|c| c.is_control()) {
        return None;
    }
    Some(format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\"")))
}

/// curl's whole configuration for one request, fed on stdin. `body` is a file to PUT.
pub fn curl_config(url: &str, token: &str, body: Option<&Path>) -> Option<String> {
    let mut c = String::new();
    c.push_str(&format!("url = {}\n", quoted(url)?));
    c.push_str(&format!("header = {}\n", quoted(&format!("authorization: GX {token}"))?));
    c.push_str("header = \"x-gx-client: launcher\"\n");
    c.push_str("silent\nshow-error\nproto = \"=https,http\"\nmax-time = 20\nmax-filesize = 65536\n");
    c.push_str("write-out = \"\\n%{http_code}\"\n");
    if let Some(p) = body {
        c.push_str("request = \"PUT\"\n");
        c.push_str("header = \"content-type: application/octet-stream\"\n");
        c.push_str(&format!("data-binary = {}\n", quoted(&format!("@{}", p.to_str()?))?));
    }
    Some(c)
}

/// One request. Ok((status, body)) whenever curl ran to an answer.
fn request(url: &str, token: &str, body: Option<&Path>) -> Result<(u16, String), String> {
    if !url_allowed(url) {
        return Err(format!("refusing to contact '{url}'"));
    }
    let config = curl_config(url, token, body).ok_or("a value curl's config cannot carry")?;
    run_curl(&config, None)
}

/// curl with its whole configuration on stdin, never on argv (the assertion is a bearer token and any
/// local user can read argv). The config must end with `write-out = "\n%{http_code}"`. Ok((status,
/// body)) whenever curl ran to an answer. Also `crashlogs`' upload, which runs it in `cwd` so that its
/// config names the body by a bare file name (a folder path need not be text the config can carry).
pub(crate) fn run_curl(config: &str, cwd: Option<&Path>) -> Result<(u16, String), String> {
    use std::io::Write;
    let mut cmd = std::process::Command::new("curl");
    if let Some(d) = cwd {
        cmd.current_dir(d);
    }
    cmd.args(["-K", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = cmd.spawn().map_err(|e| format!("curl: {e}"))?;
    child.stdin.take().ok_or("curl: no stdin")?.write_all(config.as_bytes()).map_err(|e| e.to_string())?;
    let out = child.wait_with_output().map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    let (body, code) = text.rsplit_once('\n').unwrap_or(("", text.as_str()));
    match code.trim().parse::<u16>() {
        Ok(c) if c > 0 => Ok((c, body.to_string())),
        _ => Err(String::from_utf8_lossy(&out.stderr).trim().to_string()),
    }
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Report {
    pub wanted: usize,
    pub offered: usize,
    pub stored: usize,
    pub failed: usize,
}

/// Ask, intersect, upload. `maps` is the folder to offer from; `base` the Worker's origin.
pub fn sync_with(base: &str, token: &str, maps: &Path) -> Result<Report, String> {
    let local = local_previews(maps);
    if local.is_empty() {
        return Ok(Report::default()); // nothing to offer: no request at all
    }
    let (code, body) = request(&format!("{base}/mappreview/wanted"), token, None)?;
    if code != 200 {
        return Err(format!("wanted list: HTTP {code}"));
    }
    let wanted = parse_wanted(&body);
    let mut r = Report { wanted: wanted.len(), ..Default::default() };
    for s in wanted.iter().filter(|s| local.contains_key(*s)).take(MAX_PER_LAUNCH) {
        r.offered += 1;
        match request(&format!("{base}/mappreview/{s}"), token, Some(&local[s])) {
            Ok((200 | 201, _)) => r.stored += 1,
            // Out of budget, signed out or banned: the rest would be refused the same way.
            Ok((401 | 403 | 429, _)) => {
                r.failed += 1;
                break;
            }
            _ => r.failed += 1,
        }
    }
    Ok(r)
}

/// The signed-in player's live assertion from `<userdata>/GXLogin.txt`, or None (signed out, expired,
/// no user data folder). Also what `crashlogs` sends with a crash log.
pub fn login() -> Option<String> {
    let ud = crate::release::engine_user_data_dir()?;
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    std::fs::read_to_string(ud.join(LOGIN_FILE)).ok().and_then(|t| login_token(&t, now))
}

/// The account of the last sign-in on this computer, live or expired: who answered a question asked while
/// they were signed in, even if the sign-in lapsed before the answer (`crashlogs::for_account`).
pub fn last_login() -> Option<String> {
    let ud = crate::release::engine_user_data_dir()?;
    // now = 0: every expiry is in the future, so only the shape is checked.
    std::fs::read_to_string(ud.join(LOGIN_FILE)).ok().and_then(|t| login_token(&t, 0))
}

/// The launcher's once-per-launch call. Signed out, or no user data folder: does nothing.
pub fn sync() -> Result<Report, String> {
    let Some(ud) = crate::release::engine_user_data_dir() else { return Ok(Report::default()) };
    let Some(token) = login() else {
        return Ok(Report::default());
    };
    sync_with(BASE_URL, &token, &ud.join("Maps"))
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../../tools/admin/panel/test/fixtures/map-slug-vectors.tsv");

    #[test]
    fn slug_matches_the_shared_vectors() {
        let mut n = 0;
        for line in VECTORS.lines().filter(|l| !l.starts_with('#') && !l.is_empty()) {
            let (leaf, want) = line.split_once('\t').expect("leaf<TAB>slug");
            let leaf = leaf.replace("\\t", "\t");
            let want = if want.is_empty() { None } else { Some(want.to_string()) };
            assert_eq!(slug(&leaf), want, "leaf {leaf:?}");
            n += 1;
        }
        assert!(n >= 12, "the vector file has {n} rows");
    }

    #[test]
    fn is_slug_is_the_workers_shape() {
        for s in ["a", "twilight-flame", "0", &"x".repeat(64)] {
            assert!(is_slug(s), "{s}");
        }
        for s in ["", "-a", "a-", "a--b", "A", "a b", "a.b", &"x".repeat(65)] {
            assert!(!is_slug(s), "{s}");
        }
    }

    fn scratch(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("gxio-mappreview-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn local_previews_finds_maps_with_a_tga_and_nothing_else() {
        let d = scratch("walk");
        std::fs::create_dir_all(d.join("Dust Devil")).unwrap();
        std::fs::write(d.join("Dust Devil/Dust Devil.map"), b"m").unwrap();
        std::fs::write(d.join("Dust Devil/Dust Devil.tga"), b"t").unwrap();
        std::fs::create_dir_all(d.join("NoPic")).unwrap();
        std::fs::write(d.join("NoPic/NoPic.map"), b"m").unwrap();
        std::fs::write(d.join("Loose.map"), b"m").unwrap();
        std::fs::write(d.join("Loose.tga"), b"t").unwrap();
        std::fs::create_dir_all(d.join("Huge")).unwrap();
        std::fs::write(d.join("Huge/Huge.map"), b"m").unwrap();
        std::fs::write(d.join("Huge/Huge.tga"), vec![0u8; MAX_BYTES as usize + 1]).unwrap();
        std::fs::write(d.join("MapCache.ini"), b"x").unwrap();
        // A map pack's extensionless CJK note: its last four bytes straddle a char. Must be skipped, not panic.
        std::fs::write(d.join("テスト"), b"x").unwrap();
        std::fs::write(d.join("Dust Devil/说明"), b"x").unwrap();
        let got = local_previews(&d);
        assert_eq!(got.keys().cloned().collect::<Vec<_>>(), vec!["dust-devil".to_string(), "loose".to_string()]);
        assert!(got["dust-devil"].ends_with("Dust Devil/Dust Devil.tga"));
        assert!(local_previews(&d.join("absent")).is_empty());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn login_token_takes_a_live_player_assertion_only() {
        let sig = "A".repeat(86);
        let live = format!("1~190000000000000001~mapper~1000~9999999999~{sig}");
        assert_eq!(login_token(&format!("{live}\n"), 2000).as_deref(), Some(live.as_str()));
        assert_eq!(login_token(&format!("1~1~n~1000~2000~{sig}"), 2061), None, "expired past the skew");
        assert!(login_token(&format!("1~1~n~1000~2000~{sig}"), 0).is_some(), "now = 0 checks only the shape (last_login)");
        assert_eq!(login_token(&format!("1~1~n~1000~2000~b~k~{sig}"), 0), None, "...and still refuses a bot");
        assert_eq!(login_token(&format!("1~1~n~1000~2000~b~k~{sig}"), 1500), None, "a bot assertion");
        assert_eq!(login_token("", 0), None);
        assert_eq!(login_token("1~a b~n~1~2~s", 0), None);
    }

    #[test]
    fn parse_wanted_is_strict() {
        assert_eq!(parse_wanted(r#"{"ok":true,"wanted":["a","dust-devil"]}"#), vec!["a", "dust-devil"]);
        assert!(parse_wanted(r#"{"ok":true,"wanted":[]}"#).is_empty());
        assert!(parse_wanted(r#"{"ok":true,"wanted":["a","../x"]}"#).is_empty());
        assert!(parse_wanted("<html>").is_empty());
    }

    #[test]
    fn the_token_is_never_on_argv_and_the_config_escapes() {
        let c = curl_config("https://e.test/mappreview/x", "1~2~n~3~4~sig", Some(Path::new(r#"C:\Maps\a "b"\a.tga"#))).unwrap();
        assert!(c.contains("header = \"authorization: GX 1~2~n~3~4~sig\"\n"), "{c}");
        assert!(c.contains(r#"data-binary = "@C:\\Maps\\a \"b\"\\a.tga""#), "{c}");
        assert!(c.contains("request = \"PUT\"\n"));
        assert!(curl_config("https://e.test/x\n-o /tmp/p", "t", None).is_none(), "a newline cannot inject an option");
        assert!(!url_allowed("http://e.test/x") && !url_allowed("file:///etc/passwd") && url_allowed("https://e.test/x"));
    }

    /// A tiny HTTP server on 127.0.0.1 answering `answers` in order; hands back every raw request.
    fn server(answers: Vec<(&'static str, String)>) -> (u16, std::thread::JoinHandle<Vec<String>>) {
        use std::io::{Read, Write};
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let h = std::thread::spawn(move || {
            let mut seen = Vec::new();
            for (status, body) in answers {
                let (mut s, _) = l.accept().unwrap();
                s.set_read_timeout(Some(std::time::Duration::from_secs(10))).unwrap();
                let mut buf = Vec::new();
                let mut chunk = [0u8; 8192];
                loop {
                    let n = s.read(&mut chunk).unwrap_or(0);
                    buf.extend_from_slice(&chunk[..n]);
                    let text = String::from_utf8_lossy(&buf).to_string();
                    if let Some(end) = text.find("\r\n\r\n") {
                        let len = text.lines().find_map(|l| l.to_ascii_lowercase().strip_prefix("content-length:")
                            .map(|v| v.trim().parse::<usize>().unwrap_or(0))).unwrap_or(0);
                        if buf.len() >= end + 4 + len { break; }
                    }
                    if n == 0 { break; }
                }
                let _ = s.write_all(format!("HTTP/1.1 {status}\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}", body.len()).as_bytes());
                seen.push(String::from_utf8_lossy(&buf).to_string());
            }
            seen
        });
        (port, h)
    }

    #[test]
    fn sync_asks_then_uploads_only_the_wanted_maps_it_has() {
        let d = scratch("sync");
        for (dir, bytes) in [("Dust Devil", b"TGA-DUST".as_slice()), ("Mine Only", b"TGA-MINE".as_slice())] {
            std::fs::create_dir_all(d.join(dir)).unwrap();
            std::fs::write(d.join(dir).join(format!("{dir}.map")), b"m").unwrap();
            std::fs::write(d.join(dir).join(format!("{dir}.tga")), bytes).unwrap();
        }
        let (port, h) = server(vec![
            ("200 OK", r#"{"ok":true,"wanted":["someone-elses","dust-devil"]}"#.to_string()),
            ("201 Created", r#"{"ok":true,"stored":true}"#.to_string()),
        ]);
        let r = sync_with(&format!("http://127.0.0.1:{port}"), "1~2~n~3~4~sig", &d).unwrap();
        let seen = h.join().unwrap();
        assert_eq!(r, Report { wanted: 2, offered: 1, stored: 1, failed: 0 });
        assert_eq!(seen.len(), 2, "one GET and exactly one PUT");
        assert!(seen[0].starts_with("GET /mappreview/wanted "), "{}", seen[0]);
        assert!(seen[0].to_ascii_lowercase().contains("authorization: gx 1~2~n~3~4~sig"));
        assert!(seen[0].to_ascii_lowercase().contains("x-gx-client: launcher"));
        assert!(seen[1].starts_with("PUT /mappreview/dust-devil "), "{}", seen[1]);
        assert!(seen[1].ends_with("TGA-DUST"), "the map's own preview, raw: {}", seen[1]);
        assert!(!seen.iter().any(|s| s.contains("mine-only") || s.contains("TGA-MINE")), "an unwanted map is never sent");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn nothing_to_offer_makes_no_request_and_a_429_stops_the_batch() {
        let d = scratch("empty");
        // No listener at all: a request would fail, so Ok(default) proves none was made.
        assert_eq!(sync_with("http://127.0.0.1:9", "t", &d).unwrap(), Report::default());
        for dir in ["a1", "a2", "a3"] {
            std::fs::create_dir_all(d.join(dir)).unwrap();
            std::fs::write(d.join(dir).join(format!("{dir}.map")), b"m").unwrap();
            std::fs::write(d.join(dir).join(format!("{dir}.tga")), b"t").unwrap();
        }
        let (port, h) = server(vec![
            ("200 OK", r#"{"ok":true,"wanted":["a1","a2","a3"]}"#.to_string()),
            ("429 Too Many Requests", r#"{"ok":false}"#.to_string()),
        ]);
        let r = sync_with(&format!("http://127.0.0.1:{port}"), "t", &d).unwrap();
        assert_eq!(h.join().unwrap().len(), 2);
        assert_eq!(r, Report { wanted: 3, offered: 1, stored: 0, failed: 1 });
        let _ = std::fs::remove_dir_all(&d);
    }
}
