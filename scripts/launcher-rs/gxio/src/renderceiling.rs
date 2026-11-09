//! Source guards for the engine's render-resolution ceiling.
//!
//! WHY THESE LIVE HERE. The engine has no automated tests at all - there is no `add_test()` and no
//! `enable_testing()` anywhere in the repository - so a C++-only change to the display path has
//! nothing that can go red. `sagepatch.rs` in this same crate already solves that by reading the
//! engine's C++ with `include_str!`, and `cargo test -p gxio` is a CI gate. This is the same
//! mechanism pointed at a different file.
//!
//! WHAT THEY GUARD. On 2026-09-14 a cross-model review blocked `SDL3_ClampRenderResolutionToDisplay`
//! because its ceiling came from `SDL_GetCurrentDisplayMode` - the mode the DESKTOP happens to be
//! in, not what the hardware can present - so a 4K panel running at 1080p silently reduced a valid
//! 4K request. The replacement asks whether ANY mode of ANY attached display can present the
//! request. Nothing about that is visible in a build log, and the difference between the two is one
//! function call, so it is exactly the kind of thing that regresses quietly.
//!
//! HOW THEY MATCH, and why it is not `src.contains(..)`. A later round of the same review pointed
//! out that raw substring tests survive the very edits they exist to catch: commenting a line out
//! leaves it in the file, `if (false && f(..))` keeps the call, and a whole-file search finds a
//! statement that has moved to a different function. So every check here strips C++ comments first,
//! matches a SHAPE rather than a name, and is scoped to the body of the function that must contain
//! it.
//!
//! EVERY GUARD HAS A NEGATIVE CONTROL, and every control is the ORIGINAL text taken out of the
//! commit that carried the defect, or produced by mutating the real source in memory - never a
//! retyping. Twice on this project a "proof that a test goes red" was written against a defect of
//! the author's own spelling while the real one still passed.

// The CI paths-filter check is not about render ceilings: `sagepatch`'s guard reads engine files
// through the same filter and asserts on it the same way, so the function lives in `ciguard` and
// both modules call the one copy. Re-exported because this module's tests and callers named it
// here first.
pub use crate::ciguard::paths_filter_lists_each_source;

/// Remove `//` and `/* */` comments, leaving string and character literals alone - C and C++
/// literals, and C#'s verbatim `@"..."` literals, whose backslashes are not escapes.
///
/// Without this, `// TheWritableGlobalData->m_xResolution = pointW;` still satisfies a `contains`
/// test for that statement - the guard stays green while the behaviour is gone.
pub fn strip_cpp_comments(src: &str) -> String {
    let b = src.as_bytes();
    let mut out = String::with_capacity(src.len());
    let mut i = 0usize;
    while i < b.len() {
        match b[i] {
            b'/' if i + 1 < b.len() && b[i + 1] == b'/' => {
                while i < b.len() && b[i] != b'\n' {
                    i += 1;
                }
            }
            b'/' if i + 1 < b.len() && b[i + 1] == b'*' => {
                i += 2;
                while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                    // Keep newlines so line-oriented reading of the result still works.
                    if b[i] == b'\n' {
                        out.push('\n');
                    }
                    i += 1;
                }
                i = (i + 2).min(b.len());
            }
            // A C# verbatim string (`@"..."`, also inside `$@"..."`): no backslash escapes, and `""`
            // is a quote. Read as a C string, `@"\steamapps\"` would run on to the next quote in the
            // file and invert comment/code for everything after it. `@"` does not occur in C++.
            b'@' if i + 1 < b.len() && b[i + 1] == b'"' => {
                out.push_str("@\"");
                i += 2;
                while i < b.len() {
                    let c = b[i];
                    out.push(c as char);
                    i += 1;
                    if c == b'"' {
                        if i < b.len() && b[i] == b'"' {
                            out.push('"');
                            i += 1;
                        } else {
                            break;
                        }
                    }
                }
            }
            q @ (b'"' | b'\'') => {
                out.push(q as char);
                i += 1;
                while i < b.len() {
                    let c = b[i];
                    out.push(c as char);
                    i += 1;
                    if c == b'\\' && i < b.len() {
                        out.push(b[i] as char);
                        i += 1;
                    } else if c == q {
                        break;
                    }
                }
            }
            c => {
                out.push(c as char);
                i += 1;
            }
        }
    }
    out
}

/// Collapse runs of whitespace so a shape test does not depend on line breaks or indentation.
pub fn squash(src: &str) -> String {
    let mut out = String::with_capacity(src.len());
    let mut space = false;
    for c in src.chars() {
        if c.is_whitespace() {
            space = true;
        } else {
            if space && !out.is_empty() {
                out.push(' ');
            }
            space = false;
            out.push(c);
        }
    }
    out
}

/// The body of a C++ function whose signature line starts at column 0, up to the closing brace that
/// also starts at column 0. Deliberately strict: a signature that no longer matches returns `None`
/// rather than silently scanning the whole file.
pub fn function_body<'a>(src: &'a str, signature_prefix: &str) -> Option<&'a str> {
    let mut from = 0usize;
    loop {
        let at = from + src[from..].find(signature_prefix)?;
        let starts_a_line = at == 0 || src.as_bytes()[at - 1] == b'\n';
        if starts_a_line {
            // A forward DECLARATION shares the signature - `void DeclineResolution();` in
            // MainMenu.cpp is at column 0 exactly like the definition - and taking it would return
            // a "body" scraped out of whatever unrelated function follows. Whichever of `;` and `{`
            // comes first decides which one this is.
            let tail = &src[at + signature_prefix.len()..];
            let semi = tail.find(';');
            let brace = tail.find('{');
            let is_definition = match (semi, brace) {
                (_, None) => false,
                (None, Some(_)) => true,
                (Some(s), Some(b)) => b < s,
            };
            if is_definition {
                let body = &src[at..];
                let end = body.find("\n}\n").map(|i| i + 3).unwrap_or(body.len());
                return Some(&body[..end]);
            }
        }
        from = at + signature_prefix.len();
    }
}

/// A comment-free, whitespace-normalised view of one function's body.
fn shape_of(src: &str, signature_prefix: &str, what: &str) -> Result<String, String> {
    let body = function_body(src, signature_prefix)
        .ok_or_else(|| format!("{what} is gone; nothing owns this behaviour any more"))?;
    Ok(squash(&strip_cpp_comments(body)))
}

/// Is the ceiling a CAPABILITY query rather than the current desktop mode?
pub fn ceiling_is_a_capability_query(w3ddisplay_src: &str) -> Result<(), String> {
    let body = shape_of(
        w3ddisplay_src,
        "static bool SDL3_RequestExceedsDisplayCapability(",
        "SDL3_RequestExceedsDisplayCapability",
    )?;
    if body.contains("SDL_GetCurrentDisplayMode") {
        return Err("the ceiling reads SDL_GetCurrentDisplayMode, which is the desktop mode, not a \
                    capability - this is the exact defect the 2026-09-14 review blocked"
            .to_string());
    }
    if body.contains("GetNativeDisplaySize") {
        return Err("the ceiling reads GetNativeDisplaySize, which resolves to \
                    SDL3_GetNativeDisplaySize, which is SDL_GetCurrentDisplayMode x pixel_density - \
                    the desktop mode again"
            .to_string());
    }
    if !body.contains("SDL_GetFullscreenDisplayModes") {
        return Err("the ceiling no longer enumerates SDL_GetFullscreenDisplayModes, so it is not a \
                    capability query"
            .to_string());
    }
    Ok(())
}

/// Does the fallback refuse modes that are themselves unpresentable?
///
/// Checks the SHAPE. A mention of the callee proves nothing: negating the test or dropping the
/// `continue` keeps the symbol and returns the very mode the fallback exists to reject.
pub fn fallback_rejects_unpresentable_modes(w3ddisplay_src: &str) -> Result<(), String> {
    let body = shape_of(w3ddisplay_src, "static bool findBestOfferedMode(", "findBestOfferedMode")?;
    if !body.contains("SDL3_RequestExceedsDisplayCapability") {
        return Err("findBestOfferedMode selects in POINTS and the caller then multiplies by the \
                    render density, so without re-checking capability it can return a mode that \
                    exceeds the ceiling it was chosen to respect"
            .to_string());
    }
    if body.contains("!SDL3_RequestExceedsDisplayCapability") {
        return Err("the capability test in findBestOfferedMode is NEGATED, which skips every \
                    PRESENTABLE mode and keeps the unpresentable ones"
            .to_string());
    }
    // { false, false } leaves the guarded `if` intact and never enters the rejecting pass.
    if !body.contains("const bool passes[2] = { true, false };") {
        return Err("findBestOfferedMode's first pass is no longer the REJECTING one, so the \
                    capability skip below it can never run"
            .to_string());
    }
    if !body
        .contains("if (skipUnpresentable && SDL3_RequestExceedsDisplayCapability(mx, my)) continue;")
    {
        return Err("findBestOfferedMode no longer SKIPS on the capability test; the result is used \
                    rather than rejected, so the fallback can exceed the ceiling it just measured"
            .to_string());
    }
    Ok(())
}

/// Do BOTH runtime paths that turn a stored resolution into a render resolution actually ACT on the
/// ceiling? A helper nothing calls is not a guard, and neither is one whose answer is discarded:
/// `if (false && f(..))` keeps every symbol and restores the defect.
pub fn both_runtime_paths_consult_the_ceiling(w3ddisplay_src: &str) -> Result<(), String> {
    let apply = shape_of(w3ddisplay_src, "Bool W3DDisplay::setDisplayMode(", "W3DDisplay::setDisplayMode")?;
    if !apply
        .contains("if (SDL3_RequestExceedsDisplayCapability(pointW, pointH, &capW, &capH)) {")
    {
        return Err("W3DDisplay::setDisplayMode no longer BRANCHES on the ceiling, so an oversized \
                    resolution applied from the options screen is accepted unchanged"
            .to_string());
    }
    // Branching is not correcting. A branch kept for its log line, with the snap deleted, renders
    // the oversized request exactly as before while every symbol above still matches.
    for needle in [
        "if (findBestOfferedMode(this, pointW, pointH, bestX, bestY)) {",
        "pointW = bestX;",
        "pointH = bestY;",
    ] {
        if !apply.contains(needle) {
            return Err(format!(
                "W3DDisplay::setDisplayMode branches on the ceiling but no longer applies the \
                 fallback ({needle} is gone), so the oversized request is rendered anyway"
            ));
        }
    }

    let init = shape_of(w3ddisplay_src, "void W3DDisplay::init()", "W3DDisplay::init")?;
    if !init.contains(
        "const bool tooBigForAnyDisplay = SDL3_RequestExceedsDisplayCapability(xres, yres, &capW, &capH);",
    ) {
        return Err("W3DDisplay::init no longer asks the ceiling, so an oversized resolution \
                    already on disk is accepted unchanged at startup - the case the fix exists for"
            .to_string());
    }
    // ...and the answer has to reach the decision, not just exist.
    if !init.contains("const bool mayMigrate = tooBigForAnyDisplay ||") {
        return Err("W3DDisplay::init asks the ceiling but the answer no longer feeds mayMigrate, \
                    so the startup correction never fires"
            .to_string());
    }
    // ...and the migration it gates has to survive an exact match, which is the whole point of
    // un-gating it: a doubled resolution keeps its aspect and looks perfectly shaped.
    if !init.contains("if (mayMigrate && modeCount > 0 && (tooBigForAnyDisplay || !exactMatch)) {") {
        return Err("W3DDisplay::init no longer forces the migration when the request is oversized, \
                    so an oversized resolution that IS in the offered list is honoured"
            .to_string());
    }
    if !init.contains("if (findBestOfferedMode(this, xres, yres, bestX, bestY)) {") {
        return Err("W3DDisplay::init decides to correct but no longer applies the fallback"
            .to_string());
    }
    Ok(())
}

/// Does `setDisplayMode` PUBLISH the effective point size? The options path reads these globals
/// back; if the producer stops writing them, the reader silently returns the pre-change value.
pub fn setdisplaymode_publishes_the_effective_size(w3ddisplay_src: &str) -> Result<(), String> {
    let body = shape_of(w3ddisplay_src, "Bool W3DDisplay::setDisplayMode(", "W3DDisplay::setDisplayMode")?;
    for (axis, line) in [
        ("x", "TheWritableGlobalData->m_xResolution = pointW;"),
        ("y", "TheWritableGlobalData->m_yResolution = pointH;"),
    ] {
        if !body.contains(line) {
            return Err(format!(
                "setDisplayMode does not publish the effective {axis} resolution, so the options \
                 path reads a stale global and persists a resolution that is not running"
            ));
        }
    }
    Ok(())
}

/// Scoped to `saveOptions`: a statement that has drifted to some other function in the same file
/// must not satisfy this, and an unrelated assignment elsewhere must not fail it.
pub fn effective_size_reaches_the_options_path(options_menu_src: &str) -> Result<(), String> {
    let body = shape_of(options_menu_src, "static void saveOptions()", "OptionsMenu saveOptions")?;
    // BOTH axes, every time. A y-only regression persists a mixed requested/effective resolution,
    // which is a worse state than either value alone and is what an x-only guard cannot see.
    for (axis, request, readback) in [
        ("x", "TheWritableGlobalData->m_xResolution = xres;", "xres = TheGlobalData->m_xResolution;"),
        ("y", "TheWritableGlobalData->m_yResolution = yres;", "yres = TheGlobalData->m_yResolution;"),
    ] {
        if body.contains(request) {
            return Err(format!(
                "saveOptions assigns the REQUESTED {axis} resolution into the globals after \
                 setDisplayMode, overwriting the effective value the engine just published"
            ));
        }
        if !body.contains(readback) {
            return Err(format!(
                "saveOptions no longer reads the effective {axis} resolution back out of the globals"
            ));
        }
        // Presence is not enough: a readback that has drifted below the preference write persists
        // the REQUESTED value and then quietly corrects a local nobody reads again.
        let read_at = body.find(readback).expect("just checked");
        let write_at = body
            .find("prefString.format(\"%d %d\", xres, yres );")
            .ok_or("saveOptions no longer writes the Resolution preference")?;
        if read_at > write_at {
            return Err(format!(
                "saveOptions reads the effective {axis} resolution AFTER writing the preference, \
                 so the requested value is what gets persisted"
            ));
        }
    }
    Ok(())
}

/// The revert path is the SECOND writer of the Resolution preference. A fix applied only to
/// saveOptions is undone by one decline.
pub fn effective_size_reaches_the_revert_path(main_menu_src: &str) -> Result<(), String> {
    let body = shape_of(main_menu_src, "void DeclineResolution()", "MainMenu DeclineResolution")?;
    for (axis, request, readback) in [
        (
            "x",
            "TheWritableGlobalData->m_xResolution = newDispSettings.xRes;",
            "newDispSettings.xRes = TheGlobalData->m_xResolution;",
        ),
        (
            "y",
            "TheWritableGlobalData->m_yResolution = newDispSettings.yRes;",
            "newDispSettings.yRes = TheGlobalData->m_yResolution;",
        ),
    ] {
        if body.contains(request) {
            return Err(format!(
                "DeclineResolution writes the requested {axis} resolution back over the effective one"
            ));
        }
        if !body.contains(readback) {
            return Err(format!(
                "DeclineResolution no longer reads the effective {axis} resolution back, so a \
                 decline persists a resolution that is not running"
            ));
        }
        let read_at = body.find(readback).expect("just checked");
        let write_at = body
            .find("prefString.format(\"%d %d\", newDispSettings.xRes, newDispSettings.yRes);")
            .ok_or("DeclineResolution no longer writes the Resolution preference")?;
        if read_at > write_at {
            return Err(format!(
                "DeclineResolution reads the effective {axis} resolution AFTER writing the \
                 preference, so the requested value is what gets persisted"
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const W3DDISPLAY_ZH: &str = include_str!(
        "../../../../GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    );
    const OPTIONSMENU_ZH: &str = include_str!(
        "../../../../GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp"
    );
    const MAINMENU_ZH: &str = include_str!(
        "../../../../GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp"
    );
    // The Generals twin carries the same three files and the same fix, ported in a358ed370.
    // It is read here for one reason: a guard that watches only one twin lets the other rot, and
    // this project has already shipped a twin that silently diverged. Every assertion below that
    // takes a source runs against BOTH.
    const W3DDISPLAY_G: &str = include_str!(
        "../../../../Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp"
    );
    const OPTIONSMENU_G: &str = include_str!(
        "../../../../Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp"
    );
    const MAINMENU_G: &str = include_str!(
        "../../../../Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp"
    );

    const CI_WORKFLOW: &str = include_str!("../../../../.github/workflows/launcher-rs.yml");

    const SOURCES_READ_BY_THIS_SUITE: [&str; 6] = [
        "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp",
        "GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp",
        "GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp",
        "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp",
        "Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/OptionsMenu.cpp",
        "Generals/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp",
    ];

    /// `SDL3_ClampRenderResolutionToDisplay` exactly as it stood at f6958a643, the head the review
    /// blocked. Copied out with `git show`, not reconstructed.
    const BLOCKED_CLAMP_AT_F6958A643: &str = r#"
static void SDL3_ClampRenderResolutionToDisplay(Int& pxW, Int& pxH)
{
	if (pxW <= 0 || pxH <= 0)
		return;

	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	if (!DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density))
		return;                       // nothing to compare against; leave the request alone
	if (nativeW <= 0 || nativeH <= 0)
		return;
	if (pxW <= nativeW && pxH <= nativeH)
		return;                       // drawable as asked

	// The tighter of the two ratios, so the result fits in BOTH axes.
	const Real fitW = (Real)nativeW / (Real)pxW;
	const Real fitH = (Real)nativeH / (Real)pxH;
	const Real fit = (fitW < fitH) ? fitW : fitH;

	const Int outW = (Int)((Real)pxW * fit) & ~1;
	const Int outH = (Int)((Real)pxH * fit) & ~1;
	if (outW <= 0 || outH <= 0)
		return;                       // refuse to produce a degenerate surface

	fprintf(stderr,
		"INFO: render resolution %dx%d pixels exceeds the display's %dx%d - drawing at %dx%d "
		"instead (aspect preserved)\n",
		pxW, pxH, nativeW, nativeH, outW, outH);

	pxW = outW;
	pxH = outH;
}
"#;

    /// `findBestOfferedMode` exactly as it stood at 393ae3905, before the mixed-density defect the
    /// follow-up round found.
    const BESTMODE_BEFORE_THE_CAPABILITY_SKIP: &str = r#"
static bool findBestOfferedMode(W3DDisplay* display, Int wantW, Int wantH, Int& outW, Int& outH)
{
	if (display == nullptr)
		return false;

	const Int modeCount = display->getDisplayModeCount();
	Int bestX = 0, bestY = 0;
	for (Int i = 0; i < modeCount; ++i)
	{
		Int mx = 0, my = 0, mb = 0;
		display->getDisplayModeDescription(i, &mx, &my, &mb);
		if (bestY == 0)
		{
			bestX = mx; bestY = my;
			continue;
		}
		// Prefer the smallest height that is still >= the requested one; fall back
		// to the tallest available when every mode is shorter than requested.
		const bool bestReaches = (bestY >= wantH);
		const bool candReaches = (my >= wantH);
		bool better = false;
		if (candReaches && !bestReaches)              better = true;
		else if (candReaches && bestReaches)          better = (my < bestY) || (my == bestY && mx > bestX);
		else if (!candReaches && !bestReaches)        better = (my > bestY) || (my == bestY && mx > bestX);
		if (better) { bestX = mx; bestY = my; }
	}

	if (bestY <= 0 || bestX <= 0)
		return false;

	(void)wantW;	// width follows the chosen mode; only height drives the selection
	outW = bestX;
	outH = bestY;
	return true;
}
"#;

    /// The two assignments that WERE the P2 defect, verbatim from f6958a643's saveOptions and
    /// DeclineResolution. Controls put these back into the live source rather than retyping them.
    const ORIGINAL_APPLY_X: &str = "TheWritableGlobalData->m_xResolution = xres;";
    const ORIGINAL_APPLY_Y: &str = "TheWritableGlobalData->m_yResolution = yres;";
    const ORIGINAL_REVERT_X: &str = "TheWritableGlobalData->m_xResolution = newDispSettings.xRes;";
    const ORIGINAL_REVERT_Y: &str = "TheWritableGlobalData->m_yResolution = newDispSettings.yRes;";

    /// Replace exactly once, and prove the replacement happened - a control that silently changed
    /// nothing passes for the wrong reason.
    fn mutate(src: &str, from: &str, to: &str) -> String {
        assert_eq!(src.matches(from).count(), 1, "control needs exactly one {from:?} to mutate");
        src.replace(from, to)
    }

    // ---- the matching machinery itself ---------------------------------------------------------

    #[test]
    fn comments_are_stripped_but_strings_survive() {
        let src = "a(); // b();\n/* c(); */ d();\nconst char* s = \"// not a comment\";\n";
        let out = strip_cpp_comments(src);
        assert!(out.contains("a();"));
        assert!(!out.contains("b();"), "line comment survived: {out:?}");
        assert!(!out.contains("c();"), "block comment survived: {out:?}");
        assert!(out.contains("d();"));
        assert!(out.contains("\"// not a comment\""), "a string literal was eaten: {out:?}");
    }

    /// C# VERBATIM STRINGS take no backslash escapes: `@"\steamapps\"` ends at its second quote. Read
    /// as a C string, the `\"` escapes it, the literal runs on to the NEXT quote in the file, and every
    /// comment after that is read as code and every string as a comment. GameInstall.cs has two such
    /// literals (`@"\steamapps\"`, `@"\Origin Games\"`) above the resolver the release.rs guard reads,
    /// and a `<see cref="..."/>` doc comment below them was being matched as a call site. `""` is the
    /// only escape inside one.
    #[test]
    fn a_csharp_verbatim_string_ending_in_a_backslash_does_not_swallow_the_file() {
        let src = "if (p.IndexOf(@\"\\steamapps\\\", X) >= 0) {}\n/// <see cref=\"Hidden\"/>\nvar q = @\"say \"\"hi\"\"\"; // tail\nKept();\n";
        let out = strip_cpp_comments(src);
        assert!(!out.contains("Hidden"), "a comment after a verbatim string survived: {out:?}");
        assert!(!out.contains("tail"), "a comment after a \"\"-escaped verbatim string survived: {out:?}");
        assert!(out.contains("Kept();"), "code after the verbatim strings was eaten: {out:?}");
        assert!(out.contains("@\"\\steamapps\\\""), "the verbatim literal itself was changed: {out:?}");
    }

    #[test]
    fn function_body_is_scoped_to_one_function() {
        let src = "void a()\n{\n\tX;\n}\n\nvoid b()\n{\n\tY;\n}\n";
        let a = function_body(src, "void a()").expect("a");
        assert!(a.contains("X;"));
        assert!(!a.contains("Y;"), "the body ran past its own closing brace: {a:?}");
    }

    #[test]
    fn function_body_skips_a_forward_declaration() {
        // MainMenu.cpp really does declare `void DeclineResolution();` at column 0 before defining
        // it, and taking the declaration returns a body scraped out of the next function entirely.
        let src = "void a();\nvoid filler()\n{\n\tWRONG;\n}\n\nvoid a()\n{\n\tRIGHT;\n}\n";
        let a = function_body(src, "void a()").expect("a");
        assert!(a.contains("RIGHT;"), "took the declaration: {a:?}");
        assert!(!a.contains("WRONG;"), "took the declaration: {a:?}");
    }

    // ---- the ceiling ---------------------------------------------------------------------------

    #[test]
    fn the_ceiling_is_a_capability_query_not_the_desktop_mode() {
        ceiling_is_a_capability_query(W3DDISPLAY_ZH).expect("ZH W3DDisplay.cpp");
        ceiling_is_a_capability_query(W3DDISPLAY_G).expect("Generals W3DDisplay.cpp");
    }

    #[test]
    fn the_guard_goes_red_on_the_blocked_ceiling() {
        // The control renames the blocked function and changes nothing else: it fails on where the
        // ceiling is READ FROM, not on a missing symbol. A control that only failed because the
        // name was absent would pass against any rename and prove nothing.
        let renamed = mutate(
            BLOCKED_CLAMP_AT_F6958A643,
            "static void SDL3_ClampRenderResolutionToDisplay(Int& pxW, Int& pxH)",
            "static bool SDL3_RequestExceedsDisplayCapability(Int ptW, Int ptH)",
        );
        let err = ceiling_is_a_capability_query(&renamed)
            .expect_err("the blocked ceiling must not pass this guard");
        assert!(err.contains("GetNativeDisplaySize"), "got: {err}");
    }

    // ---- the fallback --------------------------------------------------------------------------

    #[test]
    fn the_fallback_rejects_modes_it_cannot_present() {
        fallback_rejects_unpresentable_modes(W3DDISPLAY_ZH).expect("ZH W3DDisplay.cpp");
        fallback_rejects_unpresentable_modes(W3DDISPLAY_G).expect("Generals W3DDisplay.cpp");
    }

    #[test]
    fn the_fallback_guard_goes_red_before_the_capability_skip() {
        let err = fallback_rejects_unpresentable_modes(BESTMODE_BEFORE_THE_CAPABILITY_SKIP)
            .expect_err("the pre-fix selection must not pass this guard");
        assert!(err.contains("POINTS"), "got: {err}");
    }

    #[test]
    fn the_fallback_guard_goes_red_when_negated_or_ignored_or_commented_out() {
        let negated = mutate(
            W3DDISPLAY_ZH,
            "if (skipUnpresentable && SDL3_RequestExceedsDisplayCapability(mx, my))",
            "if (skipUnpresentable && !SDL3_RequestExceedsDisplayCapability(mx, my))",
        );
        let err = fallback_rejects_unpresentable_modes(&negated).expect_err("negated");
        assert!(err.contains("NEGATED"), "got: {err}");

        let commented = mutate(
            W3DDISPLAY_ZH,
            "\t\t\t\tcontinue;\t// never migrate TO something nothing attached can present",
            "\t\t\t\t// continue;",
        );
        let err = fallback_rejects_unpresentable_modes(&commented).expect_err("commented out");
        assert!(err.contains("SKIPS"), "got: {err}");
    }

    // ---- both runtime call sites ---------------------------------------------------------------

    #[test]
    fn both_runtime_paths_ask_the_ceiling() {
        both_runtime_paths_consult_the_ceiling(W3DDISPLAY_ZH).expect("ZH W3DDisplay.cpp");
        both_runtime_paths_consult_the_ceiling(W3DDISPLAY_G).expect("Generals W3DDisplay.cpp");
    }

    #[test]
    fn the_call_site_guard_goes_red_when_a_path_stops_acting_on_the_answer() {
        // Three ways to keep the symbol and lose the behaviour. All three must be caught.
        let deleted = mutate(
            W3DDISPLAY_ZH,
            "if (SDL3_RequestExceedsDisplayCapability(pointW, pointH, &capW, &capH))",
            "if (false)",
        );
        let err = both_runtime_paths_consult_the_ceiling(&deleted).expect_err("deleted call");
        assert!(err.contains("setDisplayMode"), "got: {err}");

        let short_circuited = mutate(
            W3DDISPLAY_ZH,
            "if (SDL3_RequestExceedsDisplayCapability(pointW, pointH, &capW, &capH))",
            "if (false && SDL3_RequestExceedsDisplayCapability(pointW, pointH, &capW, &capH))",
        );
        let err =
            both_runtime_paths_consult_the_ceiling(&short_circuited).expect_err("short circuited");
        assert!(err.contains("BRANCHES"), "got: {err}");

        let answer_dropped = mutate(
            W3DDISPLAY_ZH,
            "const bool mayMigrate = tooBigForAnyDisplay",
            "const bool mayMigrate = false",
        );
        let err = both_runtime_paths_consult_the_ceiling(&answer_dropped).expect_err("unused");
        assert!(err.contains("mayMigrate"), "got: {err}");
    }

    // ---- publishing the effective size ---------------------------------------------------------

    #[test]
    fn setdisplaymode_publishes_what_it_actually_set() {
        setdisplaymode_publishes_the_effective_size(W3DDISPLAY_ZH).expect("ZH W3DDisplay.cpp");
        setdisplaymode_publishes_the_effective_size(W3DDISPLAY_G).expect("Generals W3DDisplay.cpp");
    }

    #[test]
    fn the_publish_guard_goes_red_on_either_axis_including_commented_out() {
        for (line, axis) in [
            ("TheWritableGlobalData->m_xResolution = pointW;", "x"),
            ("TheWritableGlobalData->m_yResolution = pointH;", "y"),
        ] {
            let removed = mutate(W3DDISPLAY_ZH, line, "");
            let err = setdisplaymode_publishes_the_effective_size(&removed).expect_err("removed");
            assert!(err.contains(axis), "got: {err}");

            // The case a raw `contains` could never see.
            let commented = mutate(W3DDISPLAY_ZH, line, &format!("// {line}"));
            let err =
                setdisplaymode_publishes_the_effective_size(&commented).expect_err("commented");
            assert!(err.contains(axis), "commenting it out was not caught: {err}");
        }
    }

    // ---- the apply and revert paths ------------------------------------------------------------

    #[test]
    fn the_effective_resolution_reaches_the_apply_path() {
        effective_size_reaches_the_options_path(OPTIONSMENU_ZH).expect("ZH OptionsMenu.cpp");
        effective_size_reaches_the_options_path(OPTIONSMENU_G).expect("Generals OptionsMenu.cpp");
    }

    #[test]
    fn the_apply_guard_goes_red_on_the_original_assignment_either_axis() {
        for (readback, original, axis) in [
            ("xres = TheGlobalData->m_xResolution;", ORIGINAL_APPLY_X, "x"),
            ("yres = TheGlobalData->m_yResolution;", ORIGINAL_APPLY_Y, "y"),
        ] {
            let reverted = mutate(OPTIONSMENU_ZH, readback, original);
            let err = effective_size_reaches_the_options_path(&reverted).expect_err("reverted");
            assert!(err.contains("REQUESTED") && err.contains(axis), "got: {err}");
        }
    }

    #[test]
    fn the_apply_guard_is_scoped_to_saveoptions() {
        // A readback that has drifted OUT of saveOptions must not keep the guard green just because
        // the statement is still somewhere in the file.
        let moved = mutate(OPTIONSMENU_ZH, "xres = TheGlobalData->m_xResolution;", "");
        let elsewhere = format!("{moved}\nstatic void unrelated()\n{{\n\txres = TheGlobalData->m_xResolution;\n}}\n");
        let err = effective_size_reaches_the_options_path(&elsewhere)
            .expect_err("a whole-file search would pass this");
        assert!(err.contains("no longer reads"), "got: {err}");
    }

    #[test]
    fn the_revert_path_reads_the_effective_resolution_too() {
        effective_size_reaches_the_revert_path(MAINMENU_ZH).expect("ZH MainMenu.cpp");
        effective_size_reaches_the_revert_path(MAINMENU_G).expect("Generals MainMenu.cpp");
    }

    #[test]
    fn the_revert_guard_goes_red_on_the_original_assignment_either_axis() {
        for (readback, original, axis) in [
            ("newDispSettings.xRes = TheGlobalData->m_xResolution;", ORIGINAL_REVERT_X, "x"),
            ("newDispSettings.yRes = TheGlobalData->m_yResolution;", ORIGINAL_REVERT_Y, "y"),
        ] {
            let reverted = mutate(MAINMENU_ZH, readback, original);
            let err = effective_size_reaches_the_revert_path(&reverted).expect_err("reverted");
            assert!(err.contains("requested") && err.contains(axis), "got: {err}");
        }
    }

    // ---- the blocked clamp, and CI -------------------------------------------------------------

    #[test]
    fn the_new_controls_go_red() {
        // One control per round-4 finding that this commit acts on.
        let logging_only = mutate(
            W3DDISPLAY_ZH,
            "\t\t\tpointW = bestX;",
            "\t\t\t// pointW = bestX;",
        );
        let err = both_runtime_paths_consult_the_ceiling(&logging_only)
            .expect_err("a branch that logs but does not correct must fail");
        assert!(err.contains("no longer applies the fallback"), "got: {err}");

        // buildFilteredResolutions uses the same two-pass idiom, so the needle has to carry
        // enough of findBestOfferedMode's own context to be unique. mutate() asserts that.
        let unreachable = mutate(
            W3DDISPLAY_ZH,
            "\tconst Int modeCount = display->getDisplayModeCount();\n\n\tconst bool passes[2] = { true, false };",
            "\tconst Int modeCount = display->getDisplayModeCount();\n\n\tconst bool passes[2] = { false, false };",
        );
        let err = fallback_rejects_unpresentable_modes(&unreachable)
            .expect_err("an unreachable rejecting pass must fail");
        assert!(err.contains("REJECTING"), "got: {err}");

        // A commented-out YAML entry is not an active one.
        let path = SOURCES_READ_BY_THIS_SUITE[0];
        let commented = crate::ciguard::workflow_with_path_commented_out(CI_WORKFLOW, path);
        let err = paths_filter_lists_each_source(&commented, &SOURCES_READ_BY_THIS_SUITE)
            .expect_err("a commented-out paths entry must fail");
        assert!(err.contains("appears 0 times"), "got: {err}");

        // And a readback that has drifted below the preference write.
        let reordered = mutate(
            OPTIONSMENU_ZH,
            "\t\t\t\txres = TheGlobalData->m_xResolution;\n\t\t\t\tyres = TheGlobalData->m_yResolution;",
            "",
        );
        let reordered = mutate(
            &reordered,
            "\t\t\t\tTheShell->recreateWindowLayouts();",
            "\t\t\t\txres = TheGlobalData->m_xResolution;\n\t\t\t\tyres = TheGlobalData->m_yResolution;\n\t\t\t\tTheShell->recreateWindowLayouts();",
        );
        let err = effective_size_reaches_the_options_path(&reordered)
            .expect_err("a readback after the preference write must fail");
        assert!(err.contains("AFTER writing the preference"), "got: {err}");
    }

    /// NOT guarded here, deliberately: whether the enumerated modes actually DRIVE the result.
    ///
    /// A source-text guard cannot verify data flow - it would have to become a compiler. That
    /// question is answered instead by two harnesses that COMPILE the real extracted text and run
    /// it: one against this machine's displays (proving the blocked ceiling reduces 1800x1169 and
    /// this one does not), one against a synthetic mixed-density display (proving the fallback
    /// stops picking a mode it cannot present, and that a display which returns no modes makes the
    /// answer fail open). And the project's own rule - docs/WORKDIR/TESTING_RULES.md - is satisfied
    /// by looking for the new strings in the built object file rather than by a green build.
    #[test]
    fn the_blocked_clamp_is_gone_from_the_engine() {
        for (twin, src) in [("ZH", W3DDISPLAY_ZH), ("Generals", W3DDISPLAY_G)] {
            assert!(
                !squash(&strip_cpp_comments(src))
                    .replace(" (", "(")
                    .contains("static void SDL3_ClampRenderResolutionToDisplay("),
                "{twin}: the blocked clamp is defined again; it measures the desktop mode"
            );
        }
    }

    /// The two twins must not merely both PASS the checks above - they must be the same code.
    ///
    /// Passing the same assertions is a weaker property than it looks: every check in this file is
    /// a shape test, so two implementations can satisfy all of them and still differ in a tie-break,
    /// a comparison operator, or an early return. That is exactly how twins drift - not by one side
    /// losing the fix, but by one side being edited and the other not.
    ///
    /// Compared with comments stripped and whitespace squashed, so a reflow or a twin-specific note
    /// is allowed and a behavioural edit is not.
    #[test]
    fn the_two_twins_carry_the_same_ceiling_code() {
        for signature in [
            "static bool SDL3_RequestExceedsDisplayCapability(",
            "static bool findBestOfferedMode(",
        ] {
            let zh = function_body(W3DDISPLAY_ZH, signature)
                .unwrap_or_else(|| panic!("ZH W3DDisplay.cpp has no {signature}"));
            let g = function_body(W3DDISPLAY_G, signature)
                .unwrap_or_else(|| panic!("Generals W3DDisplay.cpp has no {signature}"));
            assert_eq!(
                squash(&strip_cpp_comments(zh)),
                squash(&strip_cpp_comments(g)),
                "{signature} has diverged between the twins; port the change to both"
            );
        }
    }

    /// Control for the test above: it must be able to fail. Without this, a `function_body` that
    /// silently returned the same slice for both twins would make the parity check vacuous.
    #[test]
    fn the_twin_parity_guard_goes_red_on_a_real_divergence() {
        let drifted = mutate(
            W3DDISPLAY_G,
            "if (fbW >= reqPxW && fbH >= reqPxH) { presentable = true; break; }",
            "if (fbW >= reqPxW || fbH >= reqPxH) { presentable = true; break; }",
        );
        let signature = "static bool SDL3_RequestExceedsDisplayCapability(";
        let zh = squash(&strip_cpp_comments(
            function_body(W3DDISPLAY_ZH, signature).expect("ZH body"),
        ));
        let g = squash(&strip_cpp_comments(
            function_body(&drifted, signature).expect("drifted body"),
        ));
        assert_ne!(
            zh, g,
            "the parity guard cannot see an && changed to an ||, so it proves nothing"
        );
    }

    #[test]
    fn every_source_this_guard_reads_runs_this_suite_in_ci() {
        paths_filter_lists_each_source(CI_WORKFLOW, &SOURCES_READ_BY_THIS_SUITE)
            .expect("launcher-rs.yml paths filter");
    }

    #[test]
    fn the_ci_guard_goes_red_when_pull_request_paths_lose_the_path() {
        // The hole the per-list count exists for: the path is still in the file once (parked under
        // workflow_dispatch:, which takes no paths) while pull_request.paths no longer lists it, so a
        // whole-file count passes and a pull request editing only that file runs nothing. Since
        // bc8988d4c there is no push trigger, so pull_request.paths is the ONLY thing that runs this
        // suite on an edit. Drive the REAL checker with exactly that workflow.
        let path = SOURCES_READ_BY_THIS_SUITE[0];
        let displaced = crate::ciguard::workflow_with_path_displaced_out_of_pull_request_paths(CI_WORKFLOW, path);

        let err = paths_filter_lists_each_source(&displaced, &SOURCES_READ_BY_THIS_SUITE)
            .expect_err("the per-list checker must reject what a whole-file count accepts");
        // Since ci-r9 the parked entry is also refused as a value GitHub's schema refuses (an item straight under an event).
        assert!(err.contains("appears 0 times under pull_request.paths") || err.contains("outside the subset"), "got: {err}");
        assert!(err.contains(path), "the error should name the offending path: {err}");

        // And the policy half: the old push trigger coming back is rejected for this suite's list too.
        let restored = crate::ciguard::workflow_with_push_trigger_restored(CI_WORKFLOW);
        let err = paths_filter_lists_each_source(&restored, &SOURCES_READ_BY_THIS_SUITE)
            .expect_err("a restored push: trigger must fail");
        assert!(err.contains("push: trigger is back"), "got: {err}");
    }
}
