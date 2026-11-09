//! Does the CI workflow actually run the guards that read files outside `scripts/launcher-rs/`?
//!
//! Several suites in this crate `include_str!` engine sources, the Windows loader's C#, and the
//! shipped `design/launcher` frontend. Those files are outside the crate, so `launcher-rs.yml`'s
//! `paths` filter is the only thing that decides whether editing one of them runs the tests that
//! read it. A guard that does not run is not a guard, which is why the filter itself is asserted
//! on - and why the assertion is bounded to the `pull_request.paths` list rather than counted over
//! the whole file.
//!
//! GeneralsX @tooling Claude 11/10/2026 (E · batch-0963, lead C32) THE POLICY SINCE bc8988d4c: no
//! `push:` trigger at all. wh1ter0se was billed for Actions overage on this private repo, so CI runs on
//! pull requests (filtered by `paths`) and on demand (`workflow_dispatch`); the release gate is the
//! local four-platform verify. Under that policy "an edit to a watched file runs the suite" means
//! "a PULL REQUEST that edits it runs the suite", and the checker asserts exactly that - plus that
//! a push trigger has not come back, since that is the one change that would quietly re-bill every
//! commit (and every tag: GitHub ignores `paths:` on tag pushes).
//!
//! THE CONTRACT (lead C33, 12/10/2026), which every review of this file is judged against. IN SCOPE, and all of it:
//! * the trigger contract: a pull request that edits any listed source runs this workflow, and nothing runs it on push;
//! * the top-level keys: workflow keys only, with exactly one `on:`;
//! * all of `on:`: only pull_request (only `paths:`, entries in GitHub's filter-pattern grammar, no `!`, no `${{ }}`, each
//!   source once), workflow_dispatch and workflow_call (both empty here) and schedule (a non-empty list of `cron:` entries
//!   read as GitHub reads cron);
//! * `jobs:` is a non-empty mapping - and that is all of jobs: this guard reads.
//! OUT OF SCOPE: everything inside a job (ids, runners, needs, steps, permissions values, expressions). Why: an invalid job
//! makes GitHub flag the workflow file visibly on the pull request; it is never a silent push run and never a pull request
//! that silently runs nothing, which are the two failures this guard exists for. A half-validator of jobs only invites
//! probing (Codex ci-r10/r11 found five job-level gaps in a row), so there is none.
//! Two independent gates check the in-scope part: a fail-closed line whitelist, then (in test builds - every caller is a
//! test) the same contract on the workflow as a YAML parser reads it; the jobs: check is the parser gate's (the whitelist
//! does not read jobs:).
//!
//! Lives here rather than in either consumer because it is about the workflow, not about the
//! subject a given suite guards: `renderceiling`, `sagepatch` and `release` all call it, and
//! another caller would be an engine file and a row in one array, not another copy of this function.

/// The rest of a whitelisted line after its body: nothing, trailing whitespace, or whitespace then a
/// `# comment` (trailing spaces are harmless YAML, Codex ci-r4 P3).
fn rest_is_comment(rest: &str) -> bool {
    let t = rest.trim_start_matches([' ', '\t']);
    t.is_empty() || (t.len() < rest.len() && t.starts_with('#'))
}

/// `<indent spaces><key>:<comment?>` with a plain key (`a-z`, `_`, `-`) and NO value. The only key
/// form this guard reads: a quote, an escape, an anchor or a value on the line refuses it.
fn plain_key(line: &str, indent: usize) -> Option<&str> {
    let body = line.strip_prefix(&" ".repeat(indent))?;
    let end = body.find(|c: char| !(c.is_ascii_lowercase() || c == '_' || c == '-'))?;
    let (key, rest) = body.split_at(end);
    let rest = rest.strip_prefix(':')?;
    (!key.is_empty() && rest_is_comment(rest)).then_some(key)
}

/// `<indent spaces>- "<value>"<comment?>` where the value has no quote and no backslash.
fn quoted_item<'a>(line: &'a str, indent: usize, prefix: &str) -> Option<&'a str> {
    let body = line.strip_prefix(&" ".repeat(indent))?.strip_prefix("- ")?.strip_prefix(prefix)?;
    let body = body.strip_prefix('"')?;
    let close = body.find(['"', '\\'])?;
    if body.as_bytes()[close] != b'"' {
        return None;
    }
    rest_is_comment(&body[close + 1..]).then_some(&body[..close])
}

/// A key's value (`rest` = after the colon): none, a plain scalar, or a quoted string that CLOSES on this line (Codex ci-r5
/// P3: `name: "Rust launcher tests"` is harmless). A quote left open, a `\` escape (an escaped line break continues the
/// string), a `''` escape, an anchor, alias, tag, block or flow value is refused: a string that runs on can hide a column-0 `on:`.
fn value_is_read(rest: &str) -> bool {
    if rest_is_comment(rest) {
        return true;
    }
    if !rest.starts_with([' ', '\t']) {
        return false; // a space or a tab separates the value (Codex ci-r8 P3: a tab is valid YAML)
    }
    let value = rest.trim_start_matches([' ', '\t']);
    match value.chars().next() {
        Some(q @ ('"' | '\'')) => {
            let body = &value[1..];
            let close = if q == '"' { body.find(['"', '\\']) } else { body.find('\'') };
            matches!(close, Some(c) if body.as_bytes()[c] == q as u8 && rest_is_comment(&body[c + 1..]))
        }
        Some(c) => !"&*!|>{[%@`".contains(c),
        None => true,
    }
}

/// An indented line BEFORE `on:` (inside an earlier top-level key such as `permissions:` or `env:`): only
/// `<spaces><key>:` with a value `value_is_read` accepts. A string, block scalar, flow collection or sequence opened before
/// `on:` could run past it and hold a fake column-0 `on:` block while YAML has no `on:` at all (Codex ci-r7 P2: `name:`, then
/// `  'text`, the copied block, `  '`), so nothing may open there.
fn indented_line_before_on_is_read(line: &str) -> bool {
    let body = line.trim_start_matches(' ');
    let end = body.find(|c: char| !(c.is_ascii_alphanumeric() || c == '_' || c == '-')).unwrap_or(body.len());
    body.len() < line.len() && end > 0 && body[end..].starts_with(':') && value_is_read(&body[end + 1..])
}

/// The top-level keys GitHub's workflow syntax documents (any other one makes the whole workflow invalid, so nothing runs).
/// cache-mode is the newest (Codex ci-r15 P3: it was missing, so a valid workflow failed the guard).
const WORKFLOW_KEYS: [&str; 9] = ["name", "run-name", "on", "permissions", "env", "defaults", "concurrency", "jobs", "cache-mode"];

/// A schedule entry's cron: five POSIX fields as GitHub reads them (anything else makes the whole workflow invalid - Codex
/// ci-r10 P2: `99 99 99 99 99` had five fields). Minute 0-59, hour 0-23, day of month 1-31, month 1-12 or JAN-DEC, day of
/// week 0-6 or SUN-SAT; each field a `,` list of `*`, a value or `a-b` (a <= b), each optionally followed by `/n` (n >= 1):
/// GitHub documents `20/15` (from 20, every 15 - Codex ci-r11 P3).
fn cron_is_read(cron: &str) -> bool {
    const MONTHS: [&str; 12] = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];
    const DAYS: [&str; 7] = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"];
    let limits: [(u32, u32, &[&str], u32); 5] = [(0, 59, &[], 0), (0, 23, &[], 0), (1, 31, &[], 0), (1, 12, &MONTHS, 1), (0, 6, &DAYS, 0)];
    let fields: Vec<&str> = cron.split(' ').filter(|f| !f.is_empty()).collect();
    fields.len() == 5
        && fields.iter().zip(limits).all(|(field, (lo, hi, names, first))| {
            let value = |v: &str| {
                let n = v.parse::<u32>().ok().or_else(|| names.iter().position(|m| m.eq_ignore_ascii_case(v)).map(|i| i as u32 + first));
                n.filter(|n| (lo..=hi).contains(n))
            };
            field.split(',').all(|part| {
                let (range, step) = match part.split_once('/') {
                    Some((r, s)) => (r, Some(s)),
                    None => (part, None),
                };
                let range_ok = match range.split_once('-') {
                    _ if range == "*" => true,
                    Some((a, b)) => matches!((value(a), value(b)), (Some(a), Some(b)) if a <= b),
                    None => value(range).is_some(),
                };
                range_ok && step.is_none_or(|s| s.parse::<u32>().is_ok_and(|n| n >= 1))
            })
        })
}

/// A `pull_request.paths` entry GitHub accepts (Codex ci-r12/r13 P2: an expression and an unclosed `[` passed both gates and
/// make GitHub refuse the file): not empty, no `${{ }}` expression, and the filter-pattern grammar as actionlint's path mode
/// checks it - no leading or trailing space, no `.`/`..`/`./`/`../` path, `?` and `+` only straight after a literal
/// character or a `[...]` class, and each `[...]` closed, holding more than one character or a range `a-b` with a <= b. A backslash (an escape)
/// or a control character is refused outright: this workflow uses none. A leading `!` passes here; the negation itself is
/// refused with its own message.
fn path_pattern_is_read(pattern: &str) -> bool {
    let p = pattern.strip_prefix('!').unwrap_or(pattern);
    if p.is_empty() || p.contains("${{") || p.contains('\\') || p.chars().any(char::is_control) || p.trim() != p {
        return false;
    }
    if p == "." || p == ".." || p.starts_with("./") || p.starts_with("../") {
        return false;
    }
    let chars: Vec<char> = p.chars().collect();
    let (mut i, mut after_literal) = (0, false);
    while i < chars.len() {
        match chars[i] {
            '?' | '+' if !after_literal => return false,
            '?' | '+' | '*' => after_literal = false,
            '[' => {
                let Some(len) = chars[i + 1..].iter().position(|&c| c == ']') else { return false }; // missing ]
                let class = &chars[i + 1..i + 1 + len];
                let (mut j, mut units, mut ranges) = (0, 0, 0);
                while j < class.len() {
                    // A `-` is a range only between two characters; first (`[-ab]`) or after a range (`[a-b-]`) it is a
                    // literal, as actionlint reads it (Codex ci-r15/r16 P3). `[0-]` (no range end) is refused below.
                    if class.get(j + 1) == Some(&'-') {
                        match class.get(j + 2) {
                            Some(&end) if end >= class[j] => ranges += 1, // `[---ab]`: `-` to `-` (Codex ci-r17 P3)
                            _ => return false, // `[0-]` or a reversed range
                        }
                        j += 3;
                    } else {
                        j += 1;
                    }
                    units += 1;
                }
                if units == 0 || (units == 1 && ranges == 0) {
                    return false; // `[]`, or `[x]` (which GitHub's linter refuses as useless)
                }
                i += len + 2;
                after_literal = true; // a class is one character, so `[0-9]+` is valid (GitHub's docs use it - Codex ci-r14 P3)
                continue;
            }
            _ => after_literal = true,
        }
        i += 1;
    }
    true
}

/// Is this a line with no structure (blank or a whole-line comment)?
fn is_blank_or_comment(line: &str) -> bool {
    let t = line.trim_start_matches([' ', '\t']);
    t.is_empty() || t.starts_with('#')
}

/// Does the workflow keep the no-push policy, and does every source appear exactly once in
/// `pull_request.paths`?
///
/// FAIL-CLOSED ON A WHITELIST (Codex ci-r1 and ci-r2: six P2s against two text and scanner versions -
/// quoted, anchored and `\u`-escaped keys, trigger keys spelled inside multi-line or anchored strings,
/// an escaped quote fooling comment stripping, and a negated `!` path). Resolving YAML here is not the
/// job; refusing everything this workflow does not use is. So:
/// * the root mapping starts at column 0, so every real top-level key is a column-0 line (Codex ci-r4 P2:
///   an indented root hid `  on: push` among the skipped lines while a column-0 `on:` inside a multi-line
///   string was read as the trigger; with a column-0 root that fake makes a second `on:`);
/// * every top-level line is a plain `key:` (lowercase, no quote, anchor or alias) whose value is absent, a plain
///   scalar, or a quoted string closed on the same line (no block or flow value), and there is exactly one `on:`;
/// * before `on:`, an indented line is only `key:` with such a value (Codex ci-r7 P2: a string opened before `on:` could hold
///   the whole fake block while YAML has no `on:`; nothing that can run on may open there, so the `on:` read is the real one);
/// * inside `on:` the only lines read are `  event:`, `    key:`, `      - "path"` (no quote or
///   backslash inside) and, under `schedule:`, `    - cron: "..."`, each with an optional `# comment`,
///   plus blank and comment lines. Anything else refuses the workflow with its line number.
/// * no line break YAML honours but this does not split on (bare CR, NEL, LS, PS - Codex ci-r3).
/// Then: no `push` event, and no event outside pull_request / workflow_dispatch / workflow_call / schedule;
/// `pull_request` and `workflow_dispatch` present; no duplicate event or key; nothing under `pull_request`
/// but `paths:` (no paths-ignore, branches, branches-ignore or types - each can silence a listed source);
/// no negated (`!`) pattern in `pull_request.paths`, which GitHub applies in order and which can exclude a
/// listed source; and each source exactly once in `pull_request.paths`.
///
/// Then a SECOND, independent gate (test builds, which is every caller): the same contract on the workflow as a YAML parser
/// reads it (`parsed::policy_holds`, lead C33 after Codex ci-r4..r8), so a spoof must beat both the scanner and the parser.
pub fn paths_filter_lists_each_source(workflow: &str, sources: &[&str]) -> Result<(), String> {
    // YAML breaks lines on CR, NEL, LS and PS as well as LF (Codex ci-r3 P2: `# hidden\r  push:` read as one
    // comment line here and as a live push: trigger by YAML). CRLF is fine; any other break refuses the file.
    if workflow.replace("\r\n", "\n").contains(['\r', '\u{85}', '\u{2028}', '\u{2029}']) {
        return Err("the workflow has a line break YAML honours but this guard does not split on (a bare CR, NEL, LS or \
                    PS); use LF or CRLF"
            .to_string());
    }
    // A UTF-8 byte order mark before the first key is not part of it (Codex ci-r8 P3).
    let text = workflow.strip_prefix('\u{feff}').unwrap_or(workflow);
    let lines: Vec<&str> = text.split('\n').map(|l| l.trim_end_matches('\r')).collect();

    // Top level: a column-0 root, plain keys only, exactly one on:.
    if let Some(first) = lines.iter().find(|l| !is_blank_or_comment(l)) {
        if first.starts_with([' ', '\t']) {
            return Err(format!(
                "the workflow's first key `{first}` is indented; this guard reads only a root mapping at column 0"
            ));
        }
    }
    let mut on_at = None;
    for (n, line) in lines.iter().enumerate() {
        if is_blank_or_comment(line) {
            continue;
        }
        if line.starts_with([' ', '\t']) {
            if on_at.is_none() && !indented_line_before_on_is_read(line) {
                return Err(format!(
                    "line {} `{line}` before on: is outside the subset this guard reads (an indented `key: value` only: a \
                     string or block opened before on: could hold a fake one)",
                    n + 1
                ));
            }
            continue;
        }
        if let Some(rest) = line.strip_prefix("on:") {
            if !rest_is_comment(rest) {
                return Err(if rest.contains("push") {
                    "a push: trigger is back in the workflow (inline on:); since bc8988d4c CI runs on pull_request \
                     and workflow_dispatch only (every push, tags included, would bill Actions again)"
                        .to_string()
                } else {
                    format!("`{line}` is an inline on:; this guard reads only the block form of on:")
                });
            }
            if on_at.replace(n).is_some() {
                return Err("the workflow has more than one top-level `on:`".to_string());
            }
            continue;
        }
        let end = line.find(|c: char| !(c.is_ascii_lowercase() || c == '_' || c == '-')).unwrap_or(line.len());
        let plain = end > 0 && line[end..].starts_with(':') && value_is_read(&line[end + 1..]);
        if plain && !WORKFLOW_KEYS.contains(&&line[..end]) {
            return Err(format!("top-level line {} `{line}`: `{}` is not a workflow key, so GitHub refuses the file", n + 1, &line[..end]));
        }
        if !plain {
            return Err(format!(
                "top-level line {} `{line}` is outside the subset this guard reads (plain `key:` only)",
                n + 1
            ));
        }
    }
    let on_at = on_at.ok_or("the workflow has no top-level `on:` key")?;

    // Inside on: - the whitelist, read into (event -> key -> items).
    let mut events: Vec<(&str, Vec<(Option<&str>, Vec<&str>)>)> = Vec::new();
    for (n, line) in lines.iter().enumerate().skip(on_at + 1) {
        if is_blank_or_comment(line) {
            continue;
        }
        if !line.starts_with([' ', '\t']) {
            break; // the next top-level key ends on: (a tab-indented line is refused below, not read as the end)
        }
        let outside = || format!("on: line {} `{line}` is outside the subset this guard reads", n + 1);
        if let Some(event) = plain_key(line, 2) {
            if event == "push" {
                return Err("a push: trigger is back in the workflow; since bc8988d4c CI runs on pull_request and \
                            workflow_dispatch only (every push, tags included, would bill Actions again)"
                    .to_string());
            }
            // Only the events of the policy: anything else (release, create, workflow_run, merge_group...) could
            // start runs this guard does not model, so it is refused rather than reasoned about.
            if !["pull_request", "workflow_dispatch", "workflow_call", "schedule"].contains(&event) {
                return Err(format!(
                    "on: has the event `{event}`, outside the policy (pull_request, workflow_dispatch, workflow_call, schedule)"
                ));
            }
            if events.iter().any(|(e, _)| *e == event) {
                return Err(format!("on: has `{event}` twice"));
            }
            events.push((event, vec![(None, Vec::new())]));
            continue;
        }
        // Each event's value as GitHub's schema takes it (Codex ci-r9 P2: `      - "x"` under workflow_dispatch: passed and made
        // the whole workflow invalid): only pull_request: has `key:` lines, an item belongs to a key and is not empty, and
        // schedule: holds `- cron: "<5 fields>"` entries (kept in the event's seeded slot). workflow_dispatch: and
        // workflow_call: carry nothing here; inputs would mean extending both gates.
        let (event, keys) = events.last_mut().ok_or_else(outside)?;
        if let Some(key) = plain_key(line, 4).filter(|_| *event == "pull_request") {
            if keys.iter().any(|(k, _)| *k == Some(key)) {
                return Err(format!("{event} has `{key}` twice"));
            }
            keys.push((Some(key), Vec::new()));
        } else if let Some(item) = quoted_item(line, 6, "").filter(|i| path_pattern_is_read(i)) {
            // (an expression or a malformed pattern is refused: GitHub refuses the file - Codex ci-r12/r13 P2)
            let (key, items) = keys.last_mut().expect("seeded");
            if key.is_none() {
                return Err(outside());
            }
            items.push(item);
        } else if let Some(cron) = quoted_item(line, 4, "cron: ").filter(|c| *event == "schedule" && cron_is_read(c)) {
            keys[0].1.push(cron); // the weekly network-tests schedule: read, not counted as a path
        } else {
            return Err(outside());
        }
    }
    if events.iter().any(|(e, keys)| *e == "schedule" && keys[0].1.is_empty()) {
        return Err("on: has a schedule: with no `- cron:` entry".to_string());
    }

    let pr = events
        .iter()
        .find(|(e, _)| *e == "pull_request")
        .map(|(_, keys)| keys)
        .ok_or("no pull_request: trigger in the workflow, so no pull request runs this suite")?;
    if !events.iter().any(|(e, _)| *e == "workflow_dispatch") {
        return Err("no workflow_dispatch: trigger in the workflow, so the suite cannot be run on demand".to_string());
    }
    // paths-ignore would silence the same files while paths still lists them; and any other filter (branches,
    // branches-ignore, types) can stop every pull request from running the suite while each path still counts
    // once (Codex ci-r3 P2: `branches-ignore: ["**"]`, `types: ["closed"]`). Only paths: is read.
    if pr.iter().any(|(k, _)| *k == Some("paths-ignore")) {
        return Err("pull_request uses paths-ignore:, which can silence a listed source".to_string());
    }
    if let Some((Some(other), _)) = pr.iter().find(|(k, _)| k.is_some() && *k != Some("paths")) {
        return Err(format!(
            "pull_request has `{other}:`; only `paths:` is read - a branches or types filter can stop pull requests \
             running this suite while every path still counts"
        ));
    }
    let items = pr
        .iter()
        .find(|(k, _)| *k == Some("paths"))
        .map(|(_, items)| items)
        .ok_or("pull_request has no `paths:` list, so it runs on every commit or none")?;
    if let Some(neg) = items.iter().find(|v| v.starts_with('!')) {
        return Err(format!(
            "pull_request.paths has the negated pattern \"{neg}\"; GitHub applies patterns in order, so it can \
             exclude a listed source"
        ));
    }

    for path in sources {
        let n = items.iter().filter(|v| **v == *path).count();
        if n != 1 {
            return Err(format!(
                "{path} appears {n} times under pull_request.paths in launcher-rs.yml; it must appear \
                 exactly once, or a pull request that edits only that file never runs the tests that read it"
            ));
        }
    }
    // The second, independent gate: the same contract read by a YAML parser. Every caller of this module is a test, and
    // the parser is a test-only dependency.
    #[cfg(test)]
    parsed::policy_holds(workflow, sources)?;
    Ok(())
}

/// The policy checked on the PARSED workflow (lead C33, 12/10/2026, after Codex ci-r4..r8 kept finding line-scanner blind
/// spots: an indented root, strings holding a fake `on:`, quoting, CRLF). The line whitelist stays as the first gate, so
/// a spoof now has to get past a scanner's blind spot AND a parser's leniency at once.
///
/// serde_yaml 0.9.34+deprecated (+ unsafe-libyaml 0.2.11) is a gxio [dev-dependencies] entry ONLY: no shipped binary links
/// it. Deprecated upstream (archived 2024); licences MIT OR Apache-2.0 and MIT. 0.9 resolves scalars as YAML 1.2, so `on`
/// is the string "on" (not the YAML 1.1 boolean), as GitHub reads it.
#[cfg(test)]
mod parsed {
    use serde_yaml::{Mapping, Value};

    fn mapping<'a>(v: &'a Value, what: &str) -> Result<&'a Mapping, String> {
        v.as_mapping().ok_or_else(|| format!("the parsed {what} is not a mapping"))
    }

    pub fn policy_holds(workflow: &str, sources: &[&str]) -> Result<(), String> {
        // A leading byte order mark is not content (YAML 1.2 5.2); serde_yaml's from_str reads it as text, so it goes first.
        let workflow = workflow.strip_prefix('\u{feff}').unwrap_or(workflow);
        let doc: Value = serde_yaml::from_str(workflow).map_err(|e| format!("the workflow does not parse as YAML: {e}"))?;
        let root = mapping(&doc, "workflow root")?;
        let on = root.get("on").ok_or("the parsed workflow has no `on:`, so GitHub runs it on nothing")?;
        let on = mapping(on, "`on:`")?;
        for event in on.keys() {
            let event = event.as_str().ok_or("the parsed `on:` has a key that is not a string")?;
            if event == "push" {
                return Err("the parsed workflow has a push: trigger (since bc8988d4c CI runs on pull_request and \
                            workflow_dispatch only)"
                    .to_string());
            }
            if !["pull_request", "workflow_dispatch", "workflow_call", "schedule"].contains(&event) {
                return Err(format!("the parsed `on:` has the event `{event}`, outside the policy"));
            }
        }
        if on.get("workflow_dispatch").is_none() {
            return Err("the parsed `on:` has no workflow_dispatch".to_string());
        }
        // Each event's value as GitHub's schema takes it (Codex ci-r9 P2): dispatch and call empty here, schedule a non-empty
        // list of { cron: "<5 fields>" }; pull_request is checked below.
        for event in ["workflow_dispatch", "workflow_call"] {
            if on.get(event).is_some_and(|v| !v.is_null()) {
                return Err(format!("the parsed {event}: carries a value; this workflow configures none"));
            }
        }
        if let Some(schedule) = on.get("schedule") {
            let crons = schedule.as_sequence().filter(|s| !s.is_empty()).ok_or("the parsed schedule: is not a non-empty list")?;
            for entry in crons {
                let entry = mapping(entry, "schedule entry")?;
                let cron = entry.get("cron").and_then(Value::as_str).filter(|c| entry.len() == 1 && super::cron_is_read(c));
                cron.ok_or("a parsed schedule entry is not { cron: \"<5 fields>\" }")?;
            }
        }
        // The rest of the file as the contract reads it: only workflow keys, and jobs: a non-empty mapping. What is inside a
        // job is out of scope (see the module header).
        if let Some(key) = root.keys().find(|k| !k.as_str().is_some_and(|k| super::WORKFLOW_KEYS.contains(&k))) {
            return Err(format!("the parsed workflow has the top-level key {key:?}, which GitHub refuses"));
        }
        let jobs = mapping(root.get("jobs").ok_or("the parsed workflow has no jobs:")?, "jobs:")?;
        if jobs.is_empty() {
            return Err("the parsed jobs: is empty".to_string());
        }
        let pr = mapping(on.get("pull_request").ok_or("the parsed `on:` has no pull_request")?, "pull_request:")?;
        if let Some(other) = pr.keys().find(|k| k.as_str() != Some("paths")) {
            return Err(format!("the parsed pull_request has `{other:?}`; only paths: may filter it"));
        }
        let paths = pr
            .get("paths")
            .and_then(Value::as_sequence)
            .ok_or("the parsed pull_request has no paths: list")?
            .iter()
            .map(|v| {
                // GitHub refuses the file over an expression or a malformed pattern here (Codex ci-r12/r13 P2).
                v.as_str()
                    .filter(|p| super::path_pattern_is_read(p))
                    .ok_or("a parsed pull_request.paths entry is not a filter pattern GitHub accepts")
            })
            .collect::<Result<Vec<_>, _>>()?;
        if let Some(neg) = paths.iter().find(|p| p.starts_with('!')) {
            return Err(format!("the parsed pull_request.paths has the negated pattern \"{neg}\""));
        }
        for path in sources {
            let n = paths.iter().filter(|p| **p == *path).count();
            if n != 1 {
                return Err(format!("{path} appears {n} times in the parsed pull_request.paths"));
            }
        }
        Ok(())
    }
}

/// Byte range of the `<indent><key>:` line, line break included, matched the way the checker reads
/// keys (so a trailing comment or space does not move it - Codex ci-r4 P3). Test scaffolding: panics
/// when the key is gone.
fn key_line(workflow: &str, indent: usize, key: &str) -> std::ops::Range<usize> {
    find_key_line(workflow, indent, key).unwrap_or_else(|| panic!("the {key}: anchor moved"))
}

/// `key_line`, or None when the key is not there.
fn find_key_line(workflow: &str, indent: usize, key: &str) -> Option<std::ops::Range<usize>> {
    let mut at = 0;
    for line in workflow.split_inclusive('\n') {
        if plain_key(line.trim_end_matches(['\n', '\r']), indent) == Some(key) {
            return Some(at..at + line.len());
        }
        at += line.len();
    }
    None
}

/// The text of `pull_request:`'s `paths:` list (test scaffolding for the restored-push control).
/// Comment and blank lines between `pull_request:` and `paths:` are allowed, as the checker allows them.
fn pull_request_paths_text(workflow: &str) -> String {
    let mut block = workflow[key_line(workflow, 2, "pull_request").end..]
        .lines()
        .take_while(|l| is_blank_or_comment(l) || l.starts_with("    "));
    block.by_ref().find(|l| plain_key(l, 4) == Some("paths")).expect("the pull_request: paths anchor moved");
    block.take_while(|l| is_blank_or_comment(l) || l.starts_with("      ")).map(|l| format!("{l}\n")).collect()
}

/// The workflow with a `push:` trigger put back, carrying the same paths filter as `pull_request:`.
///
/// The negative control for the policy half of the checker: before bc8988d4c this WAS the shipped
/// workflow, so it is the most likely way the policy regresses (a revert, or a copy from an older
/// branch). Shared by every module that calls `paths_filter_lists_each_source`, so each one drives
/// the REAL checker with the real workflow. Panics on a moved anchor: it is test scaffolding.
pub fn workflow_with_push_trigger_restored(workflow: &str) -> String {
    let at = key_line(workflow, 2, "pull_request").start;
    let block = pull_request_paths_text(workflow);
    let push = format!("  push:\n    branches: ['**']\n    paths:\n{block}");
    let restored = format!("{}{push}{}", &workflow[..at], &workflow[at..]);
    assert_eq!(restored.matches("\n  push:\n").count(), 1, "exactly one push: trigger restored");
    restored
}

/// The lines from byte `from` on that list `path` as an item, matched the way the checker reads items (a trailing
/// space, comment or CRLF does not hide one - Codex ci-r6).
fn path_entries(workflow: &str, path: &str, from: usize) -> Vec<std::ops::Range<usize>> {
    let mut at = from;
    let mut found = Vec::new();
    for line in workflow[from..].split_inclusive('\n') {
        if quoted_item(line.trim_end_matches(['\n', '\r']), 6, "") == Some(path) {
            found.push(at..at + line.len());
        }
        at += line.len();
    }
    found
}

/// The workflow with `path`'s `pull_request.paths` entry commented out (`      # - "path"`): YAML drops it while a
/// text search still finds the path. Panics on a moved anchor: it is test scaffolding.
pub fn workflow_with_path_commented_out(workflow: &str, path: &str) -> String {
    let found = path_entries(workflow, path, key_line(workflow, 2, "pull_request").end);
    assert_eq!(found.len(), 1, "{path} should have exactly one entry after pull_request:");
    let at = found[0].start + 6;
    format!("{}# {}", &workflow[..at], &workflow[at..])
}

/// The displaced workflow: `path` removed from `pull_request.paths` and parked under
/// `workflow_dispatch:`, which takes no `paths:` key at all - so GitHub watches it on nothing while
/// a whole-file count (or a count over the `pull_request:`-to-`jobs:` slice) still finds one.
///
/// Under the no-push policy this is the hole that matters: a pull request editing only that file
/// runs nothing. Panics on a moved anchor: it is test scaffolding.
pub fn workflow_with_path_displaced_out_of_pull_request_paths(workflow: &str, path: &str) -> String {
    let entries = |wf: &str, from: usize| path_entries(wf, path, from);
    let found = entries(workflow, key_line(workflow, 2, "pull_request").end);
    assert_eq!(found.len(), 1, "{path} should have exactly one entry after pull_request:");
    let entry = &workflow[found[0].clone()];
    let without = format!("{}{}", &workflow[..found[0].start], &workflow[found[0].end..]);
    let at = key_line(&without, 2, "workflow_dispatch").end;
    let displaced = format!("{}{entry}{}", &without[..at], &without[at..]);
    assert_eq!(entries(&displaced, 0).len(), 1, "a whole-file count would still find the entry once and pass");
    displaced
}

#[cfg(test)]
mod tests {
    use super::*;

    // Listed in the workflow's own pull_request paths filter, so a PR that edits it runs this.
    const CI_WORKFLOW: &str = include_str!("../../../../.github/workflows/launcher-rs.yml");
    const A_WATCHED_PATH: &str = "GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp";

    /// `wf` with `text` inserted at byte `at`.
    fn insert_at(wf: &str, at: usize, text: &str) -> String {
        format!("{}{text}{}", &wf[..at], &wf[at..])
    }

    /// `wf` with byte range `r` removed.
    fn cut(wf: &str, r: std::ops::Range<usize>) -> String {
        format!("{}{}", &wf[..r.start], &wf[r.end..])
    }

    /// `pull_request:`'s `paths:` line.
    fn pr_paths_line(wf: &str) -> std::ops::Range<usize> {
        let pr = key_line(wf, 2, "pull_request").end;
        let r = key_line(&wf[pr..], 4, "paths");
        pr + r.start..pr + r.end
    }

    /// The last entry of `pull_request.paths`, matched the way the checker reads items (Codex ci-r6 P3: a trailing comment on
    /// the literal last line broke the controls anchored on it).
    fn last_pr_item(wf: &str) -> std::ops::Range<usize> {
        let mut at = pr_paths_line(wf).end;
        let mut last = None;
        for line in wf[at..].split_inclusive('\n') {
            if !(is_blank_or_comment(line.trim_end_matches(['\n', '\r'])) || line.starts_with("      ")) {
                break;
            }
            if quoted_item(line.trim_end_matches(['\n', '\r']), 6, "").is_some() {
                last = Some(at..at + line.len());
            }
            at += line.len();
        }
        last.expect("pull_request.paths has no entry")
    }

    /// The workflow with LF line ends and no byte order mark. The controls build their inputs with `\n` and at line starts;
    /// CRLF and a BOM themselves are the checker's business and have their own tests (Codex ci-r6 P3: a CRLF copy turned
    /// `on:\r\n` into `on:\r # triggers\n`, a bare CR; ci-r8 P3: a BOM).
    fn lf(wf: &str) -> String {
        wf.strip_prefix('\u{feff}').unwrap_or(wf).replace("\r\n", "\n")
    }

    /// The shipped workflow with a comment on every key line the controls anchor on, and a comment line between
    /// `pull_request:` and `paths:`. The same YAML; every control must work on it (Codex ci-r5 P3 #1: they found keys by
    /// literal text, so `  pull_request: # PRs` panicked one and made another a no-op).
    fn with_comments(wf: &str) -> String {
        let mut wf = lf(wf);
        for (indent, key, note) in [
            (0, "on", " # triggers"),
            (2, "pull_request", " # PRs\n    # sources read by the suite"),
            (2, "workflow_dispatch", " # by hand"),
        ] {
            let eol = key_line(&wf, indent, key).end - 1;
            wf.insert_str(eol, note);
        }
        let eol = pr_paths_line(&wf).end - 1;
        wf.insert_str(eol, " # sources");
        let eol = last_pr_item(&wf).end - 1;
        wf.insert_str(eol, " # the last entry");
        assert_eq!(wf.matches(" # ").count(), CI_WORKFLOW.matches(" # ").count() + 6, "every anchor took its comment");
        wf
    }

    /// The `<indent><key>:` line and everything under it (deeper-indented, blank or comment lines).
    fn block(wf: &str, indent: usize, key: &str) -> std::ops::Range<usize> {
        let r = key_line(wf, indent, key);
        let deeper = " ".repeat(indent + 1);
        let mut end = r.end;
        for line in wf[r.end..].split_inclusive('\n') {
            let l = line.trim_end_matches(['\n', '\r']);
            if !(is_blank_or_comment(l) || l.starts_with(&deeper)) {
                break;
            }
            end += line.len();
        }
        r.start..end
    }

    /// `wf` with byte range `what` moved to just before the `<indent><key>:` line.
    fn move_before(wf: &str, what: std::ops::Range<usize>, indent: usize, key: &str) -> String {
        let text = wf[what.clone()].to_string();
        let rest = cut(wf, what);
        insert_at(&rest, key_line(&rest, indent, key).start, &text)
    }

    /// The same YAML in another order: `permissions:` before `on:` (put there if the workflow has none), `workflow_dispatch:`
    /// before `pull_request:` (Codex ci-r7 P3: two controls assumed the shipped order).
    fn reordered(wf: &str) -> String {
        let wf = lf(wf);
        let wf = match find_key_line(&wf, 0, "permissions") {
            Some(_) => move_before(&wf, block(&wf, 0, "permissions"), 0, "on"),
            None => insert_at(&wf, key_line(&wf, 0, "on").start, "permissions:\n  contents: read\n"),
        };
        let wf = move_before(&wf, block(&wf, 2, "workflow_dispatch"), 2, "pull_request");
        assert!(key_line(&wf, 0, "permissions").start < key_line(&wf, 0, "on").start, "permissions: before on:");
        assert!(key_line(&wf, 2, "workflow_dispatch").start < key_line(&wf, 2, "pull_request").start, "events reordered");
        wf
    }

    /// `base` with its top-level `name:` line set to `name:{rest}` (or one put first): built on its own, so it never
    /// depends on how the shipped line is spelled (Codex ci-r9 P3).
    fn named(base: &str, rest: &str) -> String {
        match base.lines().position(|l| l.starts_with("name:")) {
            Some(n) => base.lines().enumerate().map(|(i, l)| if i == n { format!("name:{rest}\n") } else { format!("{l}\n") }).collect(),
            None => format!("name:{rest}\n{base}"),
        }
    }

    /// `base` with its schedule's cron set to `cron` (or a schedule with that cron added before workflow_dispatch:).
    fn with_cron(base: &str, cron: &str) -> String {
        let mut at = 0;
        for line in base.split_inclusive('\n') {
            if quoted_item(line.trim_end_matches('\n'), 4, "cron: ").is_some() {
                return format!("{}    - cron: \"{cron}\"\n{}", &base[..at], &base[at + line.len()..]);
            }
            at += line.len();
        }
        insert_at(base, key_line(base, 2, "workflow_dispatch").start, &format!("  schedule:\n    - cron: \"{cron}\"\n"))
    }

    /// Workflows GitHub's schema refuses although every trigger line looks right (Codex ci-r9 P2): the whole file is
    /// invalid, so no pull request runs it. Built on `base`.
    fn schema_refusals(base: &str) -> Vec<(&'static str, String)> {
        let wd = key_line(base, 2, "workflow_dispatch").end;
        vec![
            ("dispatch list", insert_at(base, wd, "      - \"x\"\n")),
            ("dispatch inputs", insert_at(base, wd, "    inputs:\n")),
            ("empty path", insert_at(base, last_pr_item(base).end, "      - \"\"\n")),
            ("bad cron", with_cron(base, "x")),
            ("unknown top-level key", insert_at(base, key_line(base, 0, "on").start, "foo: bar\n")),
            ("cron out of range", with_cron(base, "99 99 99 99 99")),
            ("cron step 0", with_cron(base, "*/0 * * * *")),
            ("no jobs", cut(base, block(base, 0, "jobs"))),
            ("expression in paths", insert_at(base, last_pr_item(base).end, "      - \"${{ github.ref }}\"\n")),
            ("unclosed class in paths", insert_at(base, last_pr_item(base).end, "      - \"[\"\n")),
        ]
    }

    /// Every control below runs on all three (LF).
    fn bases() -> [String; 3] {
        [lf(CI_WORKFLOW), with_comments(CI_WORKFLOW), reordered(CI_WORKFLOW)]
    }

    #[test]
    fn the_real_workflow_passes() {
        paths_filter_lists_each_source(CI_WORKFLOW, &[A_WATCHED_PATH, "design/launcher/**"]).expect("as shipped, byte for byte");
        for wf in bases() {
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH, "design/launcher/**"])
                .expect("the workflow this repository actually ships");
        }
    }

    #[test]
    fn a_path_displaced_out_of_pull_request_paths_goes_red() {
        // Codex's finding on the first event-split fix, as a test: an entry parked under
        // workflow_dispatch: still counted one when the pull_request slice ran to jobs:.
        for wf in bases() {
            let displaced = workflow_with_path_displaced_out_of_pull_request_paths(&wf, A_WATCHED_PATH);
            let err = paths_filter_lists_each_source(&displaced, &[A_WATCHED_PATH])
                .expect_err("a path outside pull_request.paths must fail");
            // Since ci-r9 an item straight under workflow_dispatch: is refused as a value GitHub's schema refuses.
            assert!(err.contains("appears 0 times under pull_request.paths") || err.contains("outside the subset"), "got: {err}");
            assert!(err.contains(A_WATCHED_PATH), "the error should name the path: {err}");
        }
    }

    #[test]
    fn a_push_trigger_coming_back_goes_red() {
        // Replaces `the_push_half_is_bounded_too` (the push half no longer exists). The old shipped
        // shape - push: with a branches filter and the same paths - must fail the policy, in block
        // form and in both inline forms.
        for wf in bases() {
            let restored = workflow_with_push_trigger_restored(&wf);
            let err = paths_filter_lists_each_source(&restored, &[A_WATCHED_PATH])
                .expect_err("a restored push: trigger must fail");
            assert!(err.contains("push: trigger is back"), "got: {err}");
        }

        for inline in ["on: push\n", "on: [push, pull_request, workflow_dispatch]\n"] {
            let wf = format!("name: x\n{inline}jobs:\n  t:\n    runs-on: macos-latest\n");
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH])
                .expect_err("an inline push trigger must fail");
            assert!(err.contains("push: trigger is back"), "{inline:?} got: {err}");
        }

        // A comment that merely mentions push is not a trigger (the real workflow explains the policy in one).
        for wf in bases() {
            let wf = insert_at(&wf, key_line(&wf, 0, "on").end, "  # push: is not a trigger here\n  #   push:\n");
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect("a comment is not a trigger");
        }
    }

    #[test]
    fn an_event_with_no_paths_list_goes_red() {
        // No paths: filter means the event either runs on everything or nothing - either way the
        // "exactly once" claim is not what is being asserted, so it must not silently pass.
        for wf in bases() {
            let gutted = cut(&wf, pr_paths_line(&wf).start..last_pr_item(&wf).end);
            let err = paths_filter_lists_each_source(&gutted, &[A_WATCHED_PATH])
                .expect_err("an event with no paths: list must fail");
            assert!(err.contains("no `paths:` list"), "got: {err}");
        }
    }

    #[test]
    fn a_workflow_without_a_pull_request_or_dispatch_trigger_goes_red() {
        for wf in bases() {
            let no_pr = cut(&wf, block(&wf, 2, "pull_request"));
            let err = paths_filter_lists_each_source(&no_pr, &[A_WATCHED_PATH]).expect_err("no pull_request must fail");
            assert!(err.contains("no pull_request: trigger"), "got: {err}");

            let no_dispatch = cut(&wf, block(&wf, 2, "workflow_dispatch"));
            let err = paths_filter_lists_each_source(&no_dispatch, &[A_WATCHED_PATH])
                .expect_err("no workflow_dispatch must fail");
            assert!(err.contains("no workflow_dispatch: trigger"), "got: {err}");
        }
    }

    #[test]
    fn a_crlf_copy_of_the_real_workflow_still_passes() {
        for wf in bases() {
            let crlf = wf.replace('\n', "\r\n");	// bases() are LF
            paths_filter_lists_each_source(&crlf, &[A_WATCHED_PATH, "design/launcher/**"]).expect("CRLF is the same workflow");
        }
    }

    /// Codex ci-r4 P2: an indented root. YAML reads `  on: push` (or `  on: workflow_dispatch`) as the
    /// trigger, while the column-0 `on:` the old guard read sits inside the job name's multi-line string.
    #[test]
    fn an_indented_root_is_refused() {
        for real in ["  on: push\n", "  on: workflow_dispatch\n"] {
            let wf = format!(
                "{real}  jobs:\n    test:\n      runs-on: macos-latest\n      steps:\n        - run: echo hi\n      name: 'text\n\
                 on:\n  pull_request:\n    paths:\n      - \"{A_WATCHED_PATH}\"\n  workflow_dispatch:\njobs:\n        '\n"
            );
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("an indented root must fail");
            assert!(err.contains("indented"), "{real:?} got: {err}");
        }
    }

    /// Codex ci-r4 P3 #2 and #3: trailing whitespace and comments change nothing in YAML, so they must
    /// neither fail the real workflow nor break the shared controls built from it.
    #[test]
    fn trailing_spaces_and_comments_are_harmless() {
        let base = lf(CI_WORKFLOW);
        let on_eol = key_line(&base, 0, "on").end - 1;
        for wf in [insert_at(&base, on_eol, " "), base.replace('\n', "  \n"), with_comments(&base)] {
            assert_ne!(wf, base, "the variant changed nothing");
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH, "design/launcher/**"]).expect("the same triggers");
            let err = paths_filter_lists_each_source(&workflow_with_push_trigger_restored(&wf), &[A_WATCHED_PATH])
                .expect_err("a restored push: trigger must fail");
            assert!(err.contains("push: trigger is back"), "got: {err}");
            let displaced = workflow_with_path_displaced_out_of_pull_request_paths(&wf, A_WATCHED_PATH);
            let err = paths_filter_lists_each_source(&displaced, &[A_WATCHED_PATH]).expect_err("a displaced path must fail");
            assert!(err.contains("appears 0 times") || err.contains("outside the subset"), "got: {err}");
            assert!(err.contains(A_WATCHED_PATH), "the error should name the path: {err}");
        }
    }

    /// Codex ci-r7 P2: a string opened on an indented line BEFORE on: (`name:`, then `  'text`) holds a copy of the whole on:
    /// block and closes after it; YAML has no on: at all, so GitHub runs nothing, while a scanner reads the copy. Nothing that
    /// can run on may open before on:; an indented `key: value` (permissions: moved up, as in reordered()) still may.
    #[test]
    fn a_string_opened_before_on_is_refused() {
        for wf in bases() {
            let on = block(&wf, 0, "on");
            let spoof = format!("name:\n  'text\n{}permissions:\n  '\n\n{}", &wf[on.clone()], &wf[on.end..]);
            let err = paths_filter_lists_each_source(&spoof, &[A_WATCHED_PATH]).expect_err("a fake on: inside a string must fail");
            assert!(err.contains("before on:"), "got: {err}");
            for opener in ["env:\n  A: \"x\n", "env:\n  A: |\n    x\n", "env:\n  - x\n", "env:\n  A: [x,\n    y]\n", "env:\n\tA: x\n"] {
                let wf = insert_at(&wf, key_line(&wf, 0, "on").start, opener);
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("an opener before on: must fail");
                assert!(err.contains("before on:"), "{opener:?} got: {err}");
            }
        }
        let env = "env:\n  RUST_BACKTRACE: \"1\"\n  CARGO_TERM_COLOR: always # c\n";
        let base = lf(CI_WORKFLOW);
        let wf = insert_at(&base, key_line(&base, 0, "on").start, env);
        paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect("closed one-line values before on: are metadata");
    }

    /// The parser gate ON ITS OWN (lead C33): every spoof the line scanner was shown to miss or nearly miss in Codex
    /// ci-r1..r8 is refused by the parsed policy too, so the two gates are independent; every harmless variant passes both.
    #[test]
    fn the_parsed_gate_alone_refuses_every_recorded_spoof() {
        let base = lf(CI_WORKFLOW);
        let on = block(&base, 0, "on");
        let on_end = key_line(&base, 0, "on").end;
        let spoofs = [
            ("ci-r4 indented root", format!(
                "  on: push\n  jobs:\n    test:\n      runs-on: macos-latest\n      steps:\n        - run: echo hi\n      name: 'text\n\
                 on:\n  pull_request:\n    paths:\n      - \"{A_WATCHED_PATH}\"\n  workflow_dispatch:\njobs:\n        '\n"
            )),
            ("ci-r7 string before on:", format!("name:\n  'text\n{}permissions:\n  '\n\n{}", &base[on.clone()], &base[on.end..])),
            ("ci-r3 bare CR", insert_at(&base, on_end, "  # hidden\r  push:\n")),
            ("ci-r2 escaped quote", insert_at(
                &base,
                key_line(&base, 2, "workflow_dispatch").end,
                "    inputs:\n      note:\n        description: \"\\\" # not a comment\"\n  push: {}\n",
            )),
            ("ci-r1 quoted push", insert_at(&base, on_end, "  \"push\": {}\n")),
            ("ci-r1 escaped push", insert_at(&base, on_end, "  \"\\u0070ush\": {}\n")),
            ("ci-r2 negated", insert_at(&base, last_pr_item(&base).end, "      - \"!**\"\n")),
            ("ci-r3 branches-ignore", insert_at(&base, key_line(&base, 2, "pull_request").end, "    branches-ignore:\n      - \"**\"\n")),
            ("displaced", workflow_with_path_displaced_out_of_pull_request_paths(&base, A_WATCHED_PATH)),
            ("commented out", workflow_with_path_commented_out(&base, A_WATCHED_PATH)),
            ("push restored", workflow_with_push_trigger_restored(&base)),
            ("no pull_request", cut(&base, block(&base, 2, "pull_request"))),
        ];
        let spoofs = spoofs.into_iter().chain(schema_refusals(&base));
        for (name, wf) in spoofs {
            let err = parsed::policy_holds(&wf, &[A_WATCHED_PATH]).expect_err(name);
            assert!(!err.is_empty(), "{name}");
        }
        let tabbed = named(&base, "\tRust launcher tests");
        let mut harmless: Vec<(&str, String)> = bases().into_iter().map(|b| ("base", b)).collect();
        harmless.push(("BOM", format!("\u{feff}{base}"))); // Codex ci-r8 P3: a UTF-8 BOM
        harmless.push(("tab", tabbed)); // Codex ci-r8 P3: a tab after the colon
        harmless.push(("CRLF", base.replace('\n', "\r\n")));
        for (name, wf) in harmless {
            parsed::policy_holds(&wf, &[A_WATCHED_PATH, "design/launcher/**"])
                .unwrap_or_else(|e| panic!("{name}: the same YAML must pass the parser gate: {e}"));
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH, "design/launcher/**"])
                .unwrap_or_else(|e| panic!("{name}: and both gates: {e}"));
        }
    }

    /// Codex ci-r9 P2: a value GitHub's schema refuses (a list under workflow_dispatch:, inputs, an empty path, a bad cron, an
    /// unknown top-level key) makes the whole workflow invalid, so no pull request runs it; both gates refuse each.
    #[test]
    fn a_documented_top_level_key_passes() {
        // Codex ci-r15 P3: cache-mode is a documented workflow key.
        for wf in bases() {
            let wf = insert_at(&wf, key_line(&wf, 0, "on").start, "cache-mode: read\n");
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect("a documented key is not a refusal");
        }
    }

    #[test]
    fn a_workflow_github_refuses_is_refused() {
        for wf in bases() {
            for (name, wf) in schema_refusals(&wf) {
                paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err(name);
            }
        }
    }

    /// Codex ci-r13 P2: a paths entry is read in GitHub's filter-pattern grammar (actionlint's path mode), not just as text.
    #[test]
    fn path_patterns_are_read_as_github_reads_them() {
        for good in ["GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp", "design/launcher/**", ".github/workflows/x.yml",
                     "src/[ab]c/*.rs", "src/[a-z]/x", "a+/b", "docs/**/*.md", "a?b", "with space/x", "!negated/**",
                     "src/[0-9]+.rs", "src/[0-9]?.rs", "src/[-ab].rs", "src/[a-b-].rs", "src/[---ab].rs"] {
            assert!(path_pattern_is_read(good), "{good:?} is a valid filter pattern");
        }
        for bad in ["", "[", "a[]", "a[x]b", "[0-]", "[z-a]", "[a-]", "[a--]", "[--]", "?a", "*?", "*+", " a", "a ", ".", "..", "./a", "../a",
                    "${{ github.ref }}", "a\\[", "!"] {
            assert!(!path_pattern_is_read(bad), "{bad:?} is not a valid filter pattern");
        }
        for wf in bases() {
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect("every shipped entry is a valid pattern");
        }
    }

    /// Codex ci-r10 P2: cron is read as GitHub reads it - ranges, names, lists, steps - not just as five fields.
    #[test]
    fn cron_is_read_as_github_reads_it() {
        for good in ["17 5 * * 1", "*/15 * * * *", "20/15 * * * *", "0 0 1 JAN,JUL MON-FRI", "30-59/10 2 * * 0", "0 12 * dec sun", "5,10,15 0-23/2 1-31 1-12 0-6"] {
            assert!(cron_is_read(good), "{good:?} is valid cron");
        }
        for bad in ["x", "99 99 99 99 99", "*/0 * * * *", "60 * * * *", "* 24 * * *", "* * 0 * *", "* * * 13 *", "* * * * 7",
                    "5-1 * * * *", "99/15 * * * *", "* * * * MON-FUNDAY", "1 2 3 4", "1 2 3 4 5 6", "1,,2 * * * *"] {
            assert!(!cron_is_read(bad), "{bad:?} is not valid cron");
        }
    }

    /// Codex ci-r5 P3 #2: a quoted top-level value that closes on its own line is harmless metadata. One that runs
    /// on (open quote, `\` escape, `''` escape) is refused: it could carry a column-0 `on:` inside it.
    #[test]
    fn single_line_quoted_metadata_is_read() {
        // The workflow's own top-level name: line, whatever it says now, is replaced (or one is put first).
        let base = lf(CI_WORKFLOW);
        let named = |value: &str| named(&base, &format!(" {value}"));
        for value in ["\"Rust launcher tests\"", "'Rust launcher tests' # quoted", "\"Rust launcher tests\" "] {
            let wf = named(value);
            paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect("closed one-line quotes are metadata");
        }
        for value in ["\"Rust launcher tests", "\"Rust \\\n  launcher tests\"", "'it''s'", "\"a\" b"] {
            let wf = named(value);
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a run-on or escaped quote must fail");
            assert!(err.contains("outside the subset"), "{value:?} got: {err}");
        }
    }

    /// A tab-indented line inside on: is refused, not read as the end of on: (a lenient parser could take
    /// `\tpush:` as a trigger the guard never looked at).
    #[test]
    fn a_tab_indented_line_inside_on_is_refused() {
        for wf in bases() {
            let wf = insert_at(&wf, key_line(&wf, 2, "workflow_dispatch").end, "\tpush:\n");
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a tab-indented key must fail");
            assert!(err.contains("outside the subset"), "got: {err}");
        }
    }

    /// Codex ci-r1 P2 #1 and ci-r2 P2 #2: push keys YAML resolves to `push` (quoted, anchored, escaped).
    /// Refused by the whitelist, which reads only a plain `  key:`.
    #[test]
    fn a_quoted_anchored_or_escaped_push_key_is_refused() {
        for wf in bases() {
            for key in ["  \"push\": {}\n", "  'push':\n    branches: ['**']\n", "  &event push: {}\n", "  \"\\u0070ush\": {}\n"] {
                let wf = insert_at(&wf, key_line(&wf, 0, "on").end, key);
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a resolved push key must fail");
                assert!(err.contains("outside the subset"), "{key:?} got: {err}");
            }
        }
    }

    /// Codex ci-r1 P2 #2 and ci-r2 P2 #1/#3: trigger keys or a hidden push made out of string contents
    /// (a multi-line quoted string, an anchored one, an escaped quote fooling a comment stripper) - and a
    /// folded `paths: >` string, which is not a list. All need a line outside the whitelist.
    #[test]
    fn triggers_hidden_in_strings_are_refused() {
        let r1 = "on:\n  'workflow_dispatch':\n    inputs:\n      note:\n        description: '\n  pull_request:\n    paths:\n      - \"GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp\"\n  workflow_dispatch:\n        '\njobs:\n  test:\n    runs-on: macos-latest\n";
        let r2_anchor = "on:\n  workflow_dispatch:\n    inputs:\n      note:\n        description: &note '\n  pull_request:\n    paths:\n      - \"GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp\"\n  workflow_dispatch:\n        '\n        default: *note\njobs:\n  test:\n    runs-on: macos-latest\n";
        for (name, wf) in [("r1", r1.to_string()), ("r2 anchor", r2_anchor.to_string())] {
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("string contents are not triggers");
            assert!(err.contains("outside the subset"), "{name}: got: {err}");
        }
        for wf in bases() {
            let r2_escape = insert_at(
                &wf,
                key_line(&wf, 2, "workflow_dispatch").end,
                "    inputs:\n      note:\n        description: \"\\\" # not a comment\"\n  push: {}\n",
            );
            let r = pr_paths_line(&wf);
            let folded = format!("{}    paths: >\n{}", &wf[..r.start], &wf[r.end..]);
            for (name, wf) in [("r2 escape", r2_escape), ("folded", folded)] {
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("string contents are not triggers");
                assert!(err.contains("outside the subset"), "{name}: got: {err}");
            }
        }
    }

    /// Codex ci-r2 P2 #4 (a hole the original checker had too): a later `!` pattern can exclude every
    /// listed source while each still counts once.
    #[test]
    fn a_negated_path_pattern_is_refused() {
        for wf in bases() {
            let wf = insert_at(&wf, last_pr_item(&wf).end, "      - \"!**\"\n");
            let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a negated pattern must fail");
            assert!(err.contains("negated pattern"), "got: {err}");
        }
    }

    #[test]
    fn keys_and_top_level_forms_outside_the_subset_are_refused() {
        for wf in bases() {
            for key in ["  <<: *defaults\n", "  ? push\n", "  *alias: {}\n", "    paths:\n"] {
                let wf = insert_at(&wf, key_line(&wf, 0, "on").end, key);
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("an unread key must fail closed");
                assert!(err.contains("outside the subset"), "{key:?} got: {err}");
            }

            // A fake on: inside a top-level multi-line string, before the real one.
            let on_block = &wf[block(&wf, 0, "on")];
            let hidden = format!("name: 'x\n{on_block}'\non:\n  workflow_dispatch:\njobs: {{}}\n");
            let err = paths_filter_lists_each_source(&hidden, &[A_WATCHED_PATH]).expect_err("a top-level string must fail");
            assert!(err.contains("outside the subset") || err.contains("more than one"), "got: {err}");

            let twice = format!("{wf}\non:\n  workflow_dispatch:\n");
            let err = paths_filter_lists_each_source(&twice, &[A_WATCHED_PATH]).expect_err("two on: keys must fail");
            assert!(err.contains("more than one top-level `on:`"), "got: {err}");
        }
        let flow = "on: {pull_request: {paths: [\"x\"]}, workflow_dispatch: {}}\njobs: {}\n";
        let err = paths_filter_lists_each_source(flow, &["x"]).expect_err("a flow-style on: is not read");
        assert!(err.contains("inline"), "got: {err}");
    }

    /// Codex ci-r3 P2 #1: a bare CR is a YAML line break, so `# hidden\r  push:` hid a live push (and a
    /// trailing `- "!**"`) inside what this guard read as one comment line. LS/PS/NEL too.
    #[test]
    fn a_line_break_this_guard_does_not_split_on_is_refused() {
        for wf in bases() {
            let on = key_line(&wf, 0, "on").end;
            for wf in [
                insert_at(&wf, on, "  # hidden\r  push:\n"),
                insert_at(&wf, last_pr_item(&wf).end - 1, "  # x\r      - \"!**\""),
                insert_at(&wf, on, "  # hidden\u{2028}  push:\n"),
                insert_at(&wf, on, "  # hidden\u{85}  push:\n"),
            ] {
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a hidden line break must fail");
                assert!(err.contains("line break"), "got: {err}");
            }
        }
    }

    /// Codex ci-r3 P2 #2: other pull_request filters can stop every run while each path still counts once.
    #[test]
    fn a_pull_request_filter_other_than_paths_is_refused() {
        for wf in bases() {
            for filter in ["    branches-ignore:\n      - \"**\"\n", "    types:\n      - \"closed\"\n", "    branches:\n      - \"nowhere\"\n"] {
                let wf = insert_at(&wf, key_line(&wf, 2, "pull_request").end, filter);
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a PR filter must fail");
                assert!(err.contains("only `paths:` is read"), "{filter:?} got: {err}");
            }
        }
    }

    #[test]
    fn an_event_outside_the_policy_is_refused() {
        for wf in bases() {
            for event in ["  release:\n", "  workflow_run:\n", "  merge_group:\n"] {
                let wf = insert_at(&wf, key_line(&wf, 2, "workflow_dispatch").start, event);
                let err = paths_filter_lists_each_source(&wf, &[A_WATCHED_PATH]).expect_err("a foreign event must fail");
                assert!(err.contains("outside the policy"), "{event:?} got: {err}");
            }
        }
    }
}
