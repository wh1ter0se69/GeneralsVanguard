//! GeneralsX @test Claude 02/10/2026 - what the launcher writes for texture filtering and
//! anti-aliasing is what the GAME reads.
//!
//! Read -> change -> write through the launcher's own path (`Graphics::read`, `GraphicsEdit`,
//! `save_changes`), then ask the engine's REAL preference reader - UserPreferences.cpp,
//! OptionPreferences.cpp and texturefilter.cpp compiled into `options-image-quality-parser.cpp` -
//! what it reads from that file. Every other byte of the file must come back unchanged.
//!
//! IGNORED BY DEFAULT: it needs that harness, which needs a configured engine build. Run it with
//! `scripts/test/options-image-quality-parser-test.sh`, which builds the harness and passes it in
//! `GX_OPTIONS_PARSER`. Ignored, not skipped: a run without the harness shows as ignored, never as a
//! pass.
//!
//! FAIL ARM, in the same run: the file written the way a plausible bug would write it - the menu
//! POSITION for 8x instead of the sample count, a filter name the engine does not know - must make
//! the engine read something other than what was meant. A comparison that cannot see those two
//! cannot see anything.

use gxio::options::{self as opt, GraphicsEdit, TextureFilter};
use std::path::{Path, PathBuf};
use std::process::Command;

/// Wh1ter0se's Windows Options.ini, shape and all: what `UserPreferences::write` lays down (sorted,
/// `key = value`, CRLF on Windows), with keys this launcher never reads.
const WH1TER0SE: &str = "AnisotropyLevel = 2\r\nAntiAliasing = 0\r\nArchiveReplays = yes\r\n\
FirewallPortOverride = 0\r\nGameSpyIPAddress = 203.0.113.9\r\nGamma = 50\r\nIdealStaticGameLOD = Low\r\n\
Resolution = 2560 1440\r\nStaticGameLOD = Custom\r\nTextureFilter = Bilinear\r\nUseShadowDecals = yes\r\n";

fn harness() -> PathBuf {
    PathBuf::from(std::env::var_os("GX_OPTIONS_PARSER").expect(
        "GX_OPTIONS_PARSER is not set - run scripts/test/options-image-quality-parser-test.sh",
    ))
}

/// What the engine reads from `dir/Options.ini`: (filter name, anisotropy level, AA level).
fn engine_reads(dir: &Path) -> (String, u32, u32) {
    let out = Command::new(harness()).arg(dir).output().expect("run the parser harness");
    assert!(out.status.success(), "harness: {}", String::from_utf8_lossy(&out.stderr));
    let line = String::from_utf8(out.stdout).unwrap();
    let mut f = (String::new(), 0, 0);
    for kv in line.split_whitespace() {
        match kv.split_once('=').unwrap() {
            ("TextureFilter", v) => f.0 = v.to_string(),
            ("AnisotropyLevel", v) => f.1 = v.parse().unwrap(),
            ("AntiAliasing", v) => f.2 = v.parse().unwrap(),
            other => panic!("unexpected harness field {other:?}"),
        }
    }
    f
}

fn scratch(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("gxiq-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
#[ignore = "needs the engine parser harness: scripts/test/options-image-quality-parser-test.sh"]
fn what_the_launcher_writes_is_what_the_game_reads() {
    let filters = [
        (TextureFilter::None, None),
        (TextureFilter::Point, None),
        (TextureFilter::Bilinear, None),
        (TextureFilter::Trilinear, None),
        (TextureFilter::Anisotropic, Some(2)),
        (TextureFilter::Anisotropic, Some(4)),
        (TextureFilter::Anisotropic, Some(8)),
        (TextureFilter::Anisotropic, Some(16)),
    ];
    let mut cases = 0;
    for (eol, base) in [("CRLF", WH1TER0SE.to_string()), ("LF", WH1TER0SE.replace("\r\n", "\n"))] {
        for &(filter, level) in &filters {
            for aa in opt::ANTI_ALIASING_LEVELS {
                let dir = scratch(&format!("{eol}-{}-{level:?}-{aa}", filter.as_str()));
                let path = dir.join("Options.ini");
                std::fs::write(&path, &base).unwrap();

                // READ, as the screen does: Wh1ter0se's Bilinear is the game's own Options screen saving back its
                // default, never a pick - the screen shows the default and a launch applies Anisotropic 16x.
                let g = opt::Graphics::read(&opt::load_from(&path));
                assert_eq!((g.texture_filter, g.anisotropy_level, g.anti_aliasing), (None, None, Some(0)));
                assert_eq!(engine_reads(&dir), ("Anisotropic".into(), 16, 0), "the starting file, as a launch applies it");

                // CHANGE + WRITE, through the launcher's save path.
                let e = GraphicsEdit { texture_filter: Some(filter), anisotropy_level: level, anti_aliasing: Some(aa), ..Default::default() };
                opt::save_changes(&path, &e.pairs(), &e.removals()).unwrap();

                // THE GAME READS IT.
                let want_level = level.unwrap_or(2); // untouched: Wh1ter0se's 2 stays
                assert_eq!(
                    engine_reads(&dir),
                    (filter.as_str().to_string(), want_level, aa),
                    "{eol} {filter:?} {level:?} AA {aa}: the game reads something else"
                );
                // ... and the launcher reads back the same thing, so the screen does not un-set.
                let g = opt::Graphics::read(&opt::load_from(&path));
                assert_eq!((g.texture_filter, g.anisotropy_level, g.anti_aliasing), (Some(filter), Some(want_level), Some(aa)));

                // Every other line byte-identical; the three keys changed in place, the pick marker appended.
                let after = std::fs::read_to_string(&path).unwrap();
                let (b, mut a): (Vec<&str>, Vec<&str>) = (base.split_inclusive('\n').collect(), after.split_inclusive('\n').collect());
                assert_eq!(a.pop().map(str::trim_end), Some("GXTextureFilterPicked = yes"), "{eol}: the pick marker");
                assert_eq!(a.len(), b.len(), "{eol}: a line was added or lost");
                for (x, y) in b.iter().zip(&a) {
                    let key = x.split('=').next().unwrap().trim();
                    if !["TextureFilter", "AnisotropyLevel", "AntiAliasing"].contains(&key) {
                        assert_eq!(x, y, "{eol}: an unrelated line changed");
                    }
                }
                std::fs::remove_dir_all(&dir).ok();
                cases += 1;
            }
        }
    }
    assert_eq!(cases, 2 * 8 * 4);
    eprintln!("  {cases} round trips: the game read exactly what the launcher wrote");
}

/// The READ half: files a player or another program could have left, read by the launcher
/// (`Graphics::read`, what the screen shows) and by the real engine getters - they must agree.
/// The one translation is AntiAliasing 1: the getter returns it, then `WW3D::Set_MSAA_Mode` has no
/// mode for it and takes its `default:` arm, NONE (ww3d.cpp), so 1 applies as 0.
#[test]
#[ignore = "needs the engine parser harness: scripts/test/options-image-quality-parser-test.sh"]
fn what_the_game_reads_is_what_the_launcher_shows() {
    let files = [
        "TextureFilter = Anisotropic16\nAnisotropyLevel = 12\nAntiAliasing = 3\nGXTextureFilterPicked = yes\n",
        "TextureFilter = anisotropic\nAnisotropyLevel = 1\nAntiAliasing = 1\nGXTextureFilterPicked = yes\n",
        "TextureFilter = TRILINEAR\nAnisotropyLevel = 32\nAntiAliasing = 16\nGXTextureFilterPicked = yes\n",
        "TextureFilter = 4\nAnisotropyLevel = x\nAntiAliasing = -2\nGXTextureFilterPicked = yes\n",
        "  TextureFilter   =   Point  \r\nAnisotropyLevel=   8\r\nAntiAliasing =4\r\nGXTextureFilterPicked = yes\r\n",
        // Last occurrence wins; a differently-cased key is a different key; an empty value is absent.
        "TextureFilter = None\nTextureFilter = Bilinear\ntexturefilter = Point\nAntiAliasing =\nAntiAliasing = 8\nGXTextureFilterPicked = yes\n",
        "AntiAliasing = \nAnisotropyLevel = 16\n",
        "",
        // Never picked: the game's own write-back and the iOS seed (Anisotropic, no level) - the 16x default.
        "TextureFilter = Bilinear\nAnisotropyLevel = 2\nAntiAliasing = 0\n",
        "TextureFilter = Anisotropic\n",
        // The pick marker, in the spellings getBool takes and the ones it does not.
        "TextureFilter = Point\nAnisotropyLevel = 4\nGXTextureFilterPicked = TRUE\n",
        "TextureFilter = Point\nAnisotropyLevel = 4\nGXTextureFilterPicked = no\n",
        "TextureFilter = Point\nAnisotropyLevel = 4\ngxtexturefilterpicked = yes\n",
        "AnisotropyLevel = 4\nGXTextureFilterPicked = yes\n",
    ];
    for (i, text) in files.iter().enumerate() {
        let dir = scratch(&format!("read{i}"));
        std::fs::write(dir.join("Options.ini"), text).unwrap();
        let g = opt::Graphics::read(&opt::load_from(&dir.join("Options.ini")));
        let shown = (
            g.texture_filter.unwrap_or(opt::TEXTURE_FILTER_DEFAULT).as_str().to_string(),
            g.anisotropy_level.unwrap_or(opt::ANISOTROPY_DEFAULT),
            g.anti_aliasing.unwrap_or(opt::ANTI_ALIASING_DEFAULT),
        );
        let (f, a, aa) = engine_reads(&dir);
        let applied = (f, a, if aa == 1 { 0 } else { aa });
        assert_eq!(shown, applied, "file {i} {text:?}: the launcher shows something the game does not use");
        std::fs::remove_dir_all(&dir).ok();
    }
    eprintln!("  {} hand-typed files: the launcher shows what the game reads", files.len());
}

#[test]
#[ignore = "needs the engine parser harness: scripts/test/options-image-quality-parser-test.sh"]
fn fail_arm_the_harness_sees_a_wrong_spelling() {
    // Each is a write a plausible bug would make, beside what the player chose. The game must read
    // something OTHER than the choice - or the round trip above proves nothing.
    // Each mutant is (lines set on Wh1ter0se's file, what the game then applies).
    let mutants: [(&[(&str, &str)], (&str, u32, u32)); 5] = [
        // "MSAA 8x" stored as its menu position, 3: highestBit(3) = 2.
        (&[("AntiAliasing", "3")], ("Anisotropic", 16, 2)),
        // A display label instead of the engine's name: stricmp finds nothing -> NO filtering.
        (&[("TextureFilter", "Anisotropic 16x"), ("GXTextureFilterPicked", "yes")], ("None", 2, 0)),
        // The key in the wrong case: the engine's map lookup is case-sensitive, so the file's own Bilinear stays.
        (&[("texturefilter", "Trilinear"), ("GXTextureFilterPicked", "yes")], ("Bilinear", 2, 0)),
        // A level the engine snaps: 12 is 8, not 12.
        (&[("TextureFilter", "Anisotropic"), ("GXTextureFilterPicked", "yes"), ("AnisotropyLevel", "12")], ("Anisotropic", 8, 0)),
        // A pick WITHOUT the marker is read as the old default: what a writer forgetting it gets.
        (&[("TextureFilter", "Trilinear")], ("Anisotropic", 16, 0)),
    ];
    for (i, (lines, game_reads)) in mutants.iter().enumerate() {
        let dir = scratch(&format!("mutant{i}"));
        let path = dir.join("Options.ini");
        // Straight to the file, the way a buggy writer would put it.
        let mut o = opt::Options::parse(WH1TER0SE);
        let mut text = String::new();
        for (key, value) in lines.iter() {
            if key.chars().next().unwrap().is_lowercase() {
                // A differently-cased line BESIDE the real one; set() would collapse the two.
                text.push_str(&format!("{key} = {value}\r\n"));
            } else {
                o.set(key, value);
            }
        }
        let text = o.render() + &text;
        std::fs::write(&path, text).unwrap();
        let got = engine_reads(&dir);
        assert_eq!((got.0.as_str(), got.1, got.2), *game_reads, "mutant {lines:?}");
        std::fs::remove_dir_all(&dir).ok();
    }
    eprintln!("  fail arm: 5 wrong spellings, each read by the game as something other than meant");
}
