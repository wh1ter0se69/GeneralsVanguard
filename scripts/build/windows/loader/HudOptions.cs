/* The HUD settings, and the two files they live in.
 *
 * GeneralsX @feature Claude 13/08/2026
 *
 * DELIBERATELY FREE OF WinForms AND System.Drawing. Everything here is a decision about a file
 * format that the ENGINE parses, and that is the half worth testing. The launcher targets
 * net8.0-windows and cannot run on the machine most of this was written on, so the rules that
 * matter - which key name, which case, which value spelling, what the resolution scaling does to a
 * chosen point size - are kept in a file a plain net8.0 test host can compile and execute anywhere.
 * The moment this file references a Control, that stops being true.
 *
 * TWO FILES, TWO DIFFERENT OWNERS, AND THE DIFFERENCE IS NOT COSMETIC:
 *
 *   Options.ini    belongs to the GAME. Shared with retail Zero Hour and the Generals Online
 *                  client. Read the whole file, drop only the keys being set, append them, write
 *                  every other line back untouched. See GameOptions in OptionsForm.cs - this is the
 *                  same contract, batched so that setting seven keys is one pass and not seven.
 *
 *   Data\INI\CommandMap\GXUserKeys.ini  is OURS, and it is a file the engine merges rather than
 *                  replaces. See GameCommandMap below for why that directory and not another.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace GeneralsX.Loader;

/// <summary>
/// One line of an engine preferences file, matched the way the engine's own reader would.
///
/// The implementation lives here rather than beside its first caller because there is now more
/// than one caller and a second copy of "what counts as the Resolution line" is the exact shape of
/// a pair that drifts silently.
/// </summary>
internal static class IniLine
{
    /// <summary>
    /// Write <paramref name="path"/> beside itself and swap it into place, rather than rewriting it
    /// in place. Options.ini lives in Documents, which OneDrive may sync, and a write cut short in
    /// place leaves a short file: the game then starts at 800x600 with every setting gone. Owner
    /// decision, 23/09/2026: every writer of Options.ini goes through a temp file, as the engine's
    /// own writer does (GXReplaceFile.h). A failed swap leaves the real file as it was.
    /// </summary>
    public static void WriteAllLinesReplacing(string path, IEnumerable<string> lines)
    {
        var tmp = path + ".tmp";
        File.WriteAllLines(tmp, lines, new UTF8Encoding(false));
        File.Move(tmp, path, overwrite: true);
    }

    /// <summary>
    /// True when this line assigns <paramref name="key"/>, ignoring case.
    ///
    /// CASE-INSENSITIVE ON PURPOSE, AND ONLY FOR WRITING. The engine's lookup is case-SENSITIVE -
    /// PreferenceMap is a std::map&lt;AsciiString,AsciiString&gt; whose operator&lt; is strcmp
    /// (AsciiString.h:544-548) and UserPreferences::load stores keys verbatim with no folding - so
    /// "debugmenu" is a different key from "DebugMenu" and the engine will never find it. A writer
    /// that matched case-sensitively would leave a hand-edited "resolution =" line in place beside
    /// the one it appends, which is a file a human reads as contradicting itself. A READER must not
    /// use this; see HudPrefs.ReadRaw for the reason.
    /// </summary>
    public static bool IsKey(string line, string key)
    {
        var t = line.TrimStart();
        if (!t.StartsWith(key, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        var rest = t[key.Length..].TrimStart();
        return rest.StartsWith('=');
    }

    /// <summary>
    /// True when this line assigns exactly <paramref name="key"/>, case included - what the engine
    /// actually matches.
    /// </summary>
    public static bool IsKeyExact(string line, string key)
    {
        var t = line.TrimStart();
        if (!t.StartsWith(key, StringComparison.Ordinal))
        {
            return false;
        }
        var rest = t[key.Length..].TrimStart();
        return rest.StartsWith('=');
    }

    /// <summary>The value side of an assignment line, trimmed exactly as UserPreferences::load does.</summary>
    public static string Value(string line)
    {
        var eq = line.IndexOf('=');
        return eq < 0 ? string.Empty : line[(eq + 1)..].Trim();
    }
}

/// <summary>
/// The HUD preferences, their engine defaults, and the arithmetic that turns a chosen point size
/// into the size that actually appears on screen.
///
/// Every default and every clamp here is copied from a specific line of the engine and cited, not
/// guessed. A launcher that shows "8" beside a setting the engine defaults to something else is
/// worse than one that shows nothing, because it tells the user their game is in a state it is not.
/// </summary>
internal static class HudPrefs
{
    // ---------------------------------------------------------------------------------------
    // Keys. Case is load-bearing: see IniLine.IsKey.
    // ---------------------------------------------------------------------------------------

    public const string InGameMessageKey    = "InGameMessageFontSize";
    public const string NetworkLatencyKey   = "NetworkLatencyFontSize";
    public const string RenderFpsKey        = "RenderFpsFontSize";
    public const string PlayerInfoListKey   = "PlayerInfoListFontSize";
    public const string SystemTimeKey       = "SystemTimeFontSize";
    public const string GameTimeKey         = "GameTimeFontSize";
    public const string MoneyPerMinuteKey   = "ShowMoneyPerMinute";
    public const string FontAdjustmentKey   = "ResolutionFontAdjustment";
    public const string ResolutionKey       = "Resolution";

    /// <summary>
    /// The default the engine falls back to when a HUD size key is ABSENT.
    ///
    /// 8 for the five readouts (OptionPreferences.cpp:910, 924, 938, 952, 999). Zero for the
    /// in-game message list (:978), and that zero is not "no text" - it means LEAVE THE INI AND
    /// LANGUAGE VALUE ALONE, because the shipped message size differs per language and inventing a
    /// number here would silently override localisation for everybody who never touched it.
    /// </summary>
    public const int ReadoutDefault = 8;
    public const int MessageUseGameValue = 0;

    /// <summary>
    /// The engine clamps InGameMessageFontSize to 0..72 (OptionPreferences.cpp:981-991) because
    /// FontLibrary::getFont returns nullptr above 100 and an unreachable size is a SILENT no-op.
    ///
    /// The other five are clamped at the bottom (negative becomes 0) and NOT at the top. This
    /// launcher applies the same 72 to all six anyway: a readout at 200 pt is not a setting anyone
    /// wants and the failure mode - a HUD element that simply stops drawing - looks like a bug in
    /// the game rather than like a number the user typed.
    /// </summary>
    public const int MaxFontSize = 72;

    /// <summary>
    /// The engine's compiled-in message size, used ONLY to draw something in the preview when the
    /// preference is 0. InGameUI.cpp:1168. It is very likely overridden by the language data -
    /// which lives inside the game's .big archives and which this launcher deliberately does not
    /// open - so the preview must say so rather than present this as the truth.
    /// </summary>
    public const int EngineBuiltInMessageSize = 10;

    /// <summary>The engine's built-in message font face. InGameUI.cpp:1167.</summary>
    public const string EngineBuiltInMessageFont = "Arial";

    /// <summary>
    /// The scaler the language data ships when Options.ini does not override it.
    /// GlobalLanguage.cpp:132. A language pack CAN set its own, and this launcher cannot read it.
    /// </summary>
    public const double LanguageDefaultScaler = 0.7;

    /// <summary>The resolution the game's fonts were designed at. GameDefines.h:232-233.</summary>
    public const int DesignWidth  = 800;
    public const int DesignHeight = 600;

    // ---------------------------------------------------------------------------------------
    // The resolution scaling, mirrored from the engine line for line.
    // ---------------------------------------------------------------------------------------

    /// <summary>
    /// What the engine multiplies EVERY HUD point size by before asking for a font.
    ///
    /// THIS IS THE WHOLE REASON THE PREVIEW HAD TO DO ARITHMETIC. A preview that drew the number
    /// the user typed would be wrong by roughly a factor of two at 1080p: the default method is
    /// ResolutionFontSizeMethod_ClassicNoCeiling (GlobalLanguage.h:77) which is
    /// 1 + (width/800 - 1) * scaler, so 1920 wide with the default 0.7 scaler gives 1.98. A chosen
    /// 10 is drawn at 19.
    ///
    /// <paramref name="height"/> is accepted and unused, and that is deliberate rather than
    /// sloppy: the Strict and Balanced methods DO use it, a language pack can select them, and the
    /// launcher cannot read which one is in force. Dropping the parameter would quietly encode an
    /// assumption in the signature where nobody would look for it.
    /// </summary>
    public static double ResolutionScale(int width, int height, double scaler)
    {
        if (width <= 0)
        {
            return 1.0;
        }
        var factor = width / (double)DesignWidth;
        factor = 1.0 + ((factor - 1.0) * scaler);
        // GlobalLanguage.cpp:338-339. The engine never shrinks text below its designed size.
        return factor < 1.0 ? 1.0 : factor;
    }

    /// <summary>
    /// The point size the engine will actually ask the font library for.
    /// GlobalLanguage.cpp:344-353 - REAL_TO_INT_FLOOR, so it truncates rather than rounds.
    /// </summary>
    public static int AdjustFontSize(int pointSize, int width, int height, double scaler)
    {
        if (pointSize <= 0)
        {
            return 0;
        }
        return (int)Math.Floor(pointSize * ResolutionScale(width, height, scaler));
    }

    /// <summary>
    /// Point size to pixels, the way the engine does it.
    ///
    /// render2dsentence.cpp:1506-1507 - `const int dotsPerInch = 96; // always use 96` - so the
    /// engine's point sizes are at a FIXED 96 DPI and do not follow the desktop's DPI setting. A
    /// preview drawn with a point-sized WinForms font would therefore be wrong on any scaled
    /// display, which is most laptops; drawing at this pixel size is right on all of them.
    /// </summary>
    public static int PointsToPixels(int pointSize) => (int)Math.Round(pointSize * 96.0 / 72.0);

    // ---------------------------------------------------------------------------------------
    // Reading Options.ini
    // ---------------------------------------------------------------------------------------

    /// <summary>
    /// The raw value the engine would see for <paramref name="key"/>, or null if it would see none.
    ///
    /// EXACT CASE, AND THE LAST OCCURRENCE, because that is what the engine does and this is a
    /// reader. UserPreferences::load walks the file assigning `(*this)[key] = val`, so a duplicated
    /// key resolves to the LAST line, and the map is keyed by a strcmp-ordered AsciiString, so a
    /// differently-cased line is a different key the engine will never look at. Matching loosely
    /// here would report a value the game is not using.
    /// </summary>
    public static string? ReadRaw(string optionsIni, string key)
    {
        try
        {
            if (!File.Exists(optionsIni))
            {
                return null;
            }
            string? found = null;
            foreach (var line in File.ReadAllLines(optionsIni))
            {
                if (IniLine.IsKeyExact(line, key))
                {
                    found = IniLine.Value(line);
                }
            }
            // UserPreferences::load skips a pair whose value is empty (:158), so an empty value is
            // indistinguishable from an absent key as far as the engine is concerned.
            return string.IsNullOrEmpty(found) ? null : found;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>
    /// C's atoi, which is what the engine calls on these values - it takes the leading integer and
    /// ignores the rest, and yields 0 rather than an error for text.
    ///
    /// Mirrored rather than replaced with int.TryParse because the difference is visible: TryParse
    /// rejects "12pt" and would make the launcher show a default where the game shows 12.
    /// </summary>
    public static int Atoi(string? s)
    {
        if (string.IsNullOrEmpty(s))
        {
            return 0;
        }
        var i = 0;
        while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
        var start = i;
        if (i < s.Length && (s[i] == '+' || s[i] == '-')) i++;
        var digits = i;
        while (i < s.Length && s[i] >= '0' && s[i] <= '9') i++;
        if (i == digits)
        {
            return 0;
        }
        return int.TryParse(s[start..i], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var v) ? v : 0;
    }

    /// <summary>Same idea for atof, used only by the font-adjustment scaler.</summary>
    public static double Atof(string? s)
    {
        if (string.IsNullOrEmpty(s))
        {
            return 0.0;
        }
        var i = 0;
        while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
        var start = i;
        if (i < s.Length && (s[i] == '+' || s[i] == '-')) i++;
        var seen = false;
        while (i < s.Length && s[i] >= '0' && s[i] <= '9') { i++; seen = true; }
        if (i < s.Length && s[i] == '.')
        {
            i++;
            while (i < s.Length && s[i] >= '0' && s[i] <= '9') { i++; seen = true; }
        }
        if (!seen)
        {
            return 0.0;
        }
        return double.TryParse(s[start..i], NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0.0;
    }

    /// <summary>A HUD size as the engine would compute it, clamped the way the engine clamps it.</summary>
    public static int ReadSize(string optionsIni, string key, int fallback)
    {
        var raw = ReadRaw(optionsIni, key);
        if (raw is null)
        {
            return fallback;
        }
        var v = Atoi(raw);
        if (v < 0)
        {
            v = 0;                          // every getter floors at zero
        }
        if (key == InGameMessageKey && v > MaxFontSize)
        {
            v = MaxFontSize;                // only this one has an engine-side ceiling
        }
        return v;
    }

    /// <summary>
    /// ShowMoneyPerMinute. The engine compares with stricmp against "yes" and treats EVERYTHING
    /// else as off (OptionPreferences.cpp:1029-1033) - note that this is NOT
    /// UserPreferences::getBool, so "1" and "true" both read as OFF here. Writing anything but
    /// "yes"/"no" would produce a checkbox that does not survive a round trip.
    /// </summary>
    public static bool ReadMoneyPerMinute(string optionsIni, bool fallback)
    {
        var raw = ReadRaw(optionsIni, MoneyPerMinuteKey);
        return raw is null ? fallback : string.Equals(raw, "yes", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The scaler in force, as far as this launcher can tell.
    ///
    /// getResolutionFontAdjustment (OptionPreferences.cpp:1009-1021) divides the stored percentage
    /// by 100 and returns -1 for anything negative, and GlobalLanguage.cpp:264-270 then falls back
    /// to the language data's own value. The game's own options screen writes "-100" for exactly
    /// that "not set" case (OptionsMenu.cpp:802-808), so a negative number in the file is normal
    /// and must not be read as a tiny scaler.
    /// </summary>
    public static double ReadFontScaler(string optionsIni)
    {
        var raw = ReadRaw(optionsIni, FontAdjustmentKey);
        if (raw is null)
        {
            return LanguageDefaultScaler;
        }
        var v = Atof(raw) / 100.0;
        return v < 0.0 ? LanguageDefaultScaler : v;
    }

    /// <summary>
    /// The resolution the game is set to, for the preview's arithmetic. Parsed the way
    /// OptionPreferences::getResolution does - sscanf("%d%d"), so two whitespace-separated numbers.
    /// Null when the game has never recorded one, in which case the caller must not invent one.
    /// </summary>
    public static (int Width, int Height)? ReadResolution(string optionsIni)
    {
        var raw = ReadRaw(optionsIni, ResolutionKey);
        if (raw is null)
        {
            return null;
        }
        var parts = raw.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length >= 2 &&
            int.TryParse(parts[0], out var w) && int.TryParse(parts[1], out var h) &&
            w > 0 && h > 0)
        {
            return (w, h);
        }
        return null;
    }

    // ---------------------------------------------------------------------------------------
    // Writing Options.ini
    // ---------------------------------------------------------------------------------------

    /// <summary>
    /// Set several keys in one pass, leaving every other line exactly as it was.
    ///
    /// Same filter-then-append shape as GameOptions.WriteResolution and for the same reasons: a
    /// file with two of the same key ends up with one, and appending rather than editing in place
    /// disturbs no ordering the game might care about. Batched because SAVE can change seven of
    /// these at once and seven read-modify-write cycles over a file another program may be holding
    /// is seven chances to lose the race instead of one.
    ///
    /// The emitted line is "Key = Value", which is byte-for-byte what UserPreferences::write emits
    /// ("%s = %s\n"), so nothing here is distinguishable from what the game writes itself.
    /// </summary>
    public static bool WriteKeys(string optionsIni, IReadOnlyList<KeyValuePair<string, string>> pairs)
    {
        if (pairs.Count == 0)
        {
            return true;
        }
        try
        {
            var dir = Path.GetDirectoryName(optionsIni);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var lines = File.Exists(optionsIni)
                ? new List<string>(File.ReadAllLines(optionsIni))
                : new List<string>();

            lines.RemoveAll(l =>
            {
                foreach (var p in pairs)
                {
                    if (IniLine.IsKey(l, p.Key))
                    {
                        return true;
                    }
                }
                return false;
            });

            // Trailing blank lines are dropped before appending, or the file grows a gap each run.
            while (lines.Count > 0 && lines[^1].Trim().Length == 0)
            {
                lines.RemoveAt(lines.Count - 1);
            }

            foreach (var p in pairs)
            {
                lines.Add($"{p.Key} = {p.Value}");
            }

            IniLine.WriteAllLinesReplacing(optionsIni, lines);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }
}

/// <summary>
/// GXHud.ini - where the top-left readout block sits, and the only file the engine parses with a
/// reader of its own.
///
/// EVERY RULE BELOW IS COPIED FROM ONE FUNCTION: gxHudSetting, InGameUI.cpp:1007-1044. It is a
/// hand-rolled loop, not the engine's INI reader and not UserPreferences, so none of the habits
/// learned from Options.ini transfer. The differences that actually bite:
///
///   * KEYS MATCH CASE-INSENSITIVELY (:1028, strncasecmp). Options.ini is the opposite - its map is
///     strcmp-ordered, so "hudanchorx" there would be a different key the engine never reads. Here
///     it is the same key. A reader that matched exactly would miss a hand-edited line.
///   * THE LAST MATCH WINS (:1039-1040). The loop has no break; every matching line overwrites the
///     one before. That is what makes filter-then-append safe, and it is also why a duplicate left
///     behind by a careless editor is silently in charge.
///   * A NON-NUMERIC VALUE KEEPS THE PREVIOUS ONE (:1037-1040). strtol reports "no digits" by
///     leaving end == start, and the engine treats that as "not a value" rather than as zero. So
///     "HudAnchorX =" does NOT mean 0, and a reader that used int.TryParse-or-zero would show 0
///     where the game shows 3.
///   * THERE ARE NO SECTIONS. A "[Hud]" line matches no key and is inert. Writing one would look
///     tidy and would mean nothing, so this writer does not.
///   * VALUES ARE SIGNED (:1038, strtol base 10) and the shipped default for Y is -1, so a writer
///     that emitted unsigned text could not express the default it is meant to restore.
///
/// READ ONCE PER PROCESS. hudAnchorX/hudAnchorY (:1046-1057) latch into function-local statics on
/// first use, so nothing written here reaches a running game - the page says so on screen.
///
/// BYTES, NOT TEXT, AND THAT WAS FOUND BY TESTING RATHER THAN BY READING. The engine opens the file
/// with fopen/fgets, so it never decodes anything - it compares raw bytes. Two consequences, both of
/// which a File.ReadAllLines-based reader gets wrong, and both of which were caught by running this
/// class against a verbatim copy of gxHudSetting over the same fixtures:
///
///   * A UTF-8 BOM is three BYTES in front of the first key, so the engine's compare fails and the
///     first line of the file silently does nothing. .NET's readers strip a BOM by default, so the
///     launcher would have read the value, displayed it, and been the only one of the two that
///     thought the line worked.
///   * fgets fills a 256-byte buffer (:1020-1022), so a line of 256 bytes or more is SPLIT and the
///     tail is parsed as a line of its own - which can cut a key in half and stop it matching.
///
/// Both are reproduced below by decoding as Latin-1 (one byte, one char, no transformation) and
/// chunking exactly as fgets does. The point is not that either case is likely; it is that when one
/// happens, the launcher and the game agree about what the file says.
/// </summary>
internal static class GxHud
{
    public const string AnchorXKey = "HudAnchorX";
    public const string AnchorYKey = "HudAnchorY";

    /// <summary>
    /// The desktop defaults, InGameUI.cpp:979-980. The iPad build uses 32/10 instead (:976-977)
    /// because a 3px inset puts the first digit under the screen's rounded corner - irrelevant to a
    /// Windows launcher writing into the Windows user-data folder, and noted only so that a reader
    /// comparing the two numbers does not think one of them is wrong.
    ///
    /// Y IS NEGATIVE BY DESIGN. -1 lifts the row a pixel above the top edge, which is what the
    /// retail readouts have always done; it is not a sentinel and it is not a bug to be corrected.
    /// </summary>
    public const int DefaultAnchorX = 3;
    public const int DefaultAnchorY = -1;

    /// <summary>
    /// The gap the engine puts between two readouts on the row, InGameUI.cpp:982. In game pixels,
    /// so the preview has to scale it like any other distance.
    /// </summary>
    public const int GapPx = 6;

    /// <summary>
    /// What the spin boxes allow. Wide enough to park the block anywhere on a 4K screen and to lift
    /// it off the top edge, narrow enough that a typo cannot write a value the engine's own cache
    /// sentinel would collide with: hudAnchorX latches on INT_MIN (:1048), so a file literally
    /// containing -2147483648 would be re-read from disk on every single draw call.
    /// </summary>
    public const int MinAnchor = -64;
    public const int MaxAnchor = 4000;

    /// <summary>char line[256] at InGameUI.cpp:1020, which is what fgets is given as its size.</summary>
    private const int FgetsBuffer = 256;

    private static int SkipBlanks(string s, int i)
    {
        // Space and tab only - exactly the two the engine skips at :1025 and :1031.
        while (i < s.Length && (s[i] == ' ' || s[i] == '\t'))
        {
            i++;
        }
        return i;
    }

    /// <summary>
    /// The file as one char per byte. Latin-1 is the only encoding that maps every byte to a
    /// distinct char and back unchanged, which is what makes it the right choice for reading a file
    /// nobody decodes and for writing one back without disturbing bytes we did not author.
    /// </summary>
    private static string Decode(byte[] raw) => Encoding.Latin1.GetString(raw);

    /// <summary>
    /// The lines fgets would hand the parser: everything up to and including the next newline, or
    /// 255 characters, whichever comes first. The newline is left on the end because fgets leaves it
    /// there and because nothing downstream cares - strtol stops at it either way.
    /// </summary>
    private static IEnumerable<string> EngineLines(string text)
    {
        var i = 0;
        while (i < text.Length)
        {
            // fgets stores at most size-1 characters plus the terminator it adds itself.
            var max = Math.Min(text.Length - i, FgetsBuffer - 1);
            var nl = text.IndexOf('\n', i, max);
            var len = nl >= 0 ? nl - i + 1 : max;
            yield return text.Substring(i, len);
            i += len;
        }
    }

    /// <summary>
    /// The file's real lines, terminators removed - what the WRITER works with.
    ///
    /// Deliberately NOT the chunked view above. Rewriting from chunks would permanently break a long
    /// line in half, and this writer's one promise is that it disturbs nothing it did not write. The
    /// chunking only ever affects what the launcher believes the game will READ.
    /// </summary>
    private static List<string> RawLines(string text)
    {
        var outp = new List<string>();
        var start = 0;
        for (var i = 0; i < text.Length; i++)
        {
            if (text[i] != '\n')
            {
                continue;
            }
            var end = (i > start && text[i - 1] == '\r') ? i - 1 : i;
            outp.Add(text[start..end]);
            start = i + 1;
        }
        if (start < text.Length)
        {
            outp.Add(text[start..]);        // a final line with no newline after it
        }
        return outp;
    }

    /// <summary>
    /// True when the engine's loop would treat this line as an assignment to <paramref name="key"/>.
    ///
    /// Used for READING and for the filter half of WRITING, so that a line the engine would ignore
    /// is also a line this launcher will not delete. That is the whole of "preserve unknown keys":
    /// not a list of keys to keep, but a rule that only removes what it is certain about.
    /// </summary>
    public static bool AssignsKey(string line, string key, out string valueText)
    {
        valueText = string.Empty;

        var i = SkipBlanks(line, 0);
        if (i >= line.Length)
        {
            return false;
        }
        // :1026 - a comment is skipped before the key is even compared, so "; HudAnchorX = 9" is
        // inert and must survive a rewrite.
        if (line[i] == ';' || line[i] == '#')
        {
            return false;
        }
        if (line.Length - i < key.Length)
        {
            return false;
        }
        if (string.Compare(line, i, key, 0, key.Length, StringComparison.OrdinalIgnoreCase) != 0)
        {
            return false;
        }
        i += key.Length;
        i = SkipBlanks(line, i);
        // :1032-1033 - the '=' is what stops "HudAnchorXY" from matching "HudAnchorX".
        if (i >= line.Length || line[i] != '=')
        {
            return false;
        }
        valueText = line[(i + 1)..];
        return true;
    }

    /// <summary>
    /// strtol(p, &amp;end, 10) as the engine calls it, returning false for the end == start case that
    /// makes the engine keep the value it already had.
    /// </summary>
    public static bool Strtol(string s, out int value)
    {
        value = 0;
        var i = 0;
        // strtol skips any isspace(), which is a wider set than the space-and-tab above.
        while (i < s.Length && char.IsWhiteSpace(s[i]))
        {
            i++;
        }
        var start = i;
        if (i < s.Length && (s[i] == '+' || s[i] == '-'))
        {
            i++;
        }
        var firstDigit = i;
        while (i < s.Length && s[i] >= '0' && s[i] <= '9')
        {
            i++;
        }
        if (i == firstDigit)
        {
            return false;
        }

        var text = s[start..i];
        if (!long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var v))
        {
            // Only reachable by overflowing Int64, and the substring is known to be sign-plus-digits.
            // strtol saturates rather than failing, and Win32's long is 32 bits, so the engine would
            // land on INT_MAX/INT_MIN here.
            value = text[0] == '-' ? int.MinValue : int.MaxValue;
            return true;
        }
        value = (int)Math.Clamp(v, int.MinValue, (long)int.MaxValue);
        return true;
    }

    /// <summary>
    /// The value the engine would settle on for one key, or <paramref name="fallback"/> when the
    /// file, the key or every value for it is missing or unparseable.
    /// </summary>
    public static int ReadValue(string path, string key, int fallback)
    {
        try
        {
            if (!File.Exists(path))
            {
                return fallback;
            }
            var found = fallback;
            foreach (var line in EngineLines(Decode(File.ReadAllBytes(path))))
            {
                if (AssignsKey(line, key, out var raw) && Strtol(raw, out var v))
                {
                    found = v;          // no break: the engine keeps going and the last one wins
                }
            }
            return found;
        }
        catch (IOException)
        {
            return fallback;
        }
        catch (UnauthorizedAccessException)
        {
            return fallback;
        }
    }

    /// <summary>The anchor the game will use on its next start.</summary>
    public static (int X, int Y) ReadAnchor(string path) =>
        (ReadValue(path, AnchorXKey, DefaultAnchorX), ReadValue(path, AnchorYKey, DefaultAnchorY));

    /// <summary>
    /// Write both anchor keys, leaving every other line of the file exactly as it was.
    ///
    /// BOTH KEYS, ALWAYS, even when one of them is already the default. The alternative - omitting a
    /// default so the file stays minimal - makes the file's meaning depend on a constant compiled
    /// into the engine, and this launcher would then be unable to tell "never set" from "set to the
    /// value that happens to be the default today". Two explicit lines cost nothing and say exactly
    /// what the game will do.
    ///
    /// The file is NOT deleted when the values return to default, because by then it may hold keys
    /// written by something else. Filtering our own two and appending them is the only operation
    /// here that cannot lose somebody else's line.
    ///
    /// NO BOM IS EVER ADDED, and that is load-bearing rather than tidy: fgets would hand the parser
    /// those three bytes in front of the first key and the compare at :1028 would fail, so a BOM
    /// would silently disable the first line of the file. Reading and writing both go through
    /// Latin-1, which is a straight byte passthrough - so somebody else's UTF-8 comment, or even a
    /// BOM they put there, survives this rewrite unchanged rather than being re-encoded into
    /// something the engine reads differently.
    /// </summary>
    public static bool WriteAnchor(string path, int x, int y)
    {
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var lines = File.Exists(path)
                ? RawLines(Decode(File.ReadAllBytes(path)))
                : new List<string>();

            lines.RemoveAll(l =>
                AssignsKey(l, AnchorXKey, out _) || AssignsKey(l, AnchorYKey, out _));

            while (lines.Count > 0 && lines[^1].Trim().Length == 0)
            {
                lines.RemoveAt(lines.Count - 1);
            }

            // A header only on a file we are creating. Re-adding it every save would stack copies of
            // it, and adding it to a file somebody else made would be rude about a file we do not own.
            if (lines.Count == 0)
            {
                lines.Add("; GXHud.ini - written by the GeneralsX launcher.");
                lines.Add("; Where the top-left readouts (ping, fps, tps, clock) sit, in screen pixels.");
                lines.Add($"; Defaults are {AnchorXKey} = {DefaultAnchorX} and {AnchorYKey} = {DefaultAnchorY}.");
                lines.Add("; Delete this file to go back to them. Takes effect when the game next starts.");
            }

            // One blank line between whatever was already here and the two lines we own, added after
            // the trim rather than as part of the header above. Written that way because the header
            // is added ONCE and this separator has to survive EVERY save: putting the blank in the
            // header made the file lose a line on its second write and then sit stable one line
            // shorter, which is a small thing that a round-trip test caught and a reader would not.
            if (lines.Count > 0)
            {
                lines.Add(string.Empty);
            }

            lines.Add($"{AnchorXKey} = {x.ToString(CultureInfo.InvariantCulture)}");
            lines.Add($"{AnchorYKey} = {y.ToString(CultureInfo.InvariantCulture)}");

            var body = string.Join(Environment.NewLine, lines) + Environment.NewLine;
            File.WriteAllBytes(path, Encoding.Latin1.GetBytes(body));
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }
}

/// <summary>
/// The keyboard bindings this launcher can set, and the file they go in.
///
/// THERE ARE TWO, AND THEY ARE INDEPENDENT. "HUD" turned out to mean two different things:
///   * TOGGLE_GAME_HUD hides the retail chrome - the command bar with its cameos and money, the
///     minimap, the radar panel. This is the clean view of the world.
///   * TOGGLE_HUD_OVERLAY hides the readouts this project added - ping, resends, fps, tps, the game
///     clock, the per-player cash table.
/// Either can be bound without the other and all four combinations are legal states.
///
/// TOGGLE_HUD_OVERLAY KEEPS ITS NAME even though the name no longer says which half it is. The
/// engine identifies a binding by that token, so renaming it would leave an existing
/// GXUserKeys.ini binding pointing at a command that no longer exists - the key would simply stop
/// working - or, worse, at a command that still exists and now does something else. The UI labels
/// carry the disambiguation instead.
///
/// WHY A FILE IN Data\INI\CommandMap\ AND NOT ANYWHERE ELSE. Three properties had to hold at once
/// and only this location has all three.
///
///   1. IT IS MERGED, NOT SUBSTITUTED. GameEngine.cpp:771 initialises TheMetaMap with
///      loadFileDirectory("Data\INI\CommandMap"), and INI::loadFileDirectory (INI.cpp:218-232)
///      loads "CommandMap.ini" FIRST and then every .ini inside the "CommandMap\" DIRECTORY. Each
///      block resolves through MetaMap::getMetaMapRec, which returns the EXISTING record for that
///      command and overwrites only the fields the block names (MetaEvent.cpp:702-741). So a file
///      here changes one binding and leaves the other two hundred alone. Writing a loose
///      Data\INI\CommandMap.ini instead would SHADOW the shipped one - which lives inside
///      INIZH.big, not on disk - and take every other key binding in the game with it.
///
///   2. A GAME UPDATE DOES NOT TOUCH IT. The updater unpacks the payload into a staging tree and
///      then walks THAT tree placing files (gxlaunch.c place_tree, :1831-1860). It never
///      enumerates the install, so a file the payload does not contain is never considered, let
///      alone deleted. Today's Windows payload ships exactly two things under Data\ -
///      Data\INI\Multiplayer.ini and Data\English\Generals.csf (package-windows-share.ps1:142-154)
///      - so the name below cannot collide with it either.
///
///   3. THE ENGINE LOOKS HERE. GeneralsX.cmd does `cd /d "%~dp0"` before starting the game
///      (GeneralsX.iss:749-751), so the working directory IS the install directory, and the
///      engine's data paths are working-directory-relative.
///
/// AND IT CANNOT COST ANYBODY A MATCH, which is the property that had to be checked before writing
/// a single byte into a data directory. dataID is m_iniCRC mixed with a digest of loose simulation
/// files (SimulationId.h:52-53), and this file is outside BOTH halves:
///   * m_iniCRC. initSubsystem is handed a NULL Xfer for TheMetaMap (GameEngine.cpp:771), and
///     SubsystemInterfaceList::initSubsystem passes that same pointer straight to
///     loadFileDirectory (SubsystemInterface.cpp:181, :186). xferCRC is only closed into m_iniCRC
///     afterwards, at :802-803. Nothing about a CommandMap file reaches it.
///   * The loose digest. That is an explicit allow-list, not a directory scan: two .scb globs and
///     Data\INI\*.ini narrowed by SIMID_ALLOW_COMMANDBAR to commandbutton/commandset only, plus
///     five by-name probes (SimulationId.cpp:343-403). "commandmap" appears nowhere in
///     SimulationId.cpp or SimulationId.h.
/// </summary>
internal static class GameCommandMap
{
    /// <summary>The meta-event that hides every HUD readout at once. MetaEvent.cpp:162.</summary>
    public const string Command = "TOGGLE_HUD_OVERLAY";

    /// <summary>The meta-event that hides the retail chrome. MetaEvent.cpp:163.</summary>
    public const string GameHudCommand = "TOGGLE_GAME_HUD";

    /// <summary>
    /// One bindable command as the page needs to render it. The order of <see cref="All"/> is the
    /// order the rows appear in, game HUD first because that is the one that was actually asked for.
    /// </summary>
    internal sealed record Binding(
        string Command,
        string Title,
        string SuggestedKey,
        string SuggestedModifiers,
        string[] Notes);

    /// <summary>
    /// SUGGESTIONS AVOID THE RETAIL COMMAND MAP. Read out of the shipped archive rather than
    /// guessed: Data\English\CommandMap.ini inside EnglishZH.big is the file that actually carries
    /// the bindings (Data\INI\CommandMap.ini in INIZH.big is a comment header and nothing else),
    /// and across its 96 blocks it uses F1-F9 and F12 but never F10 or F11. F10 and F11 appear only
    /// in CommandMapDebug.ini, which GameEngine.cpp:773-775 loads only under RTS_DEBUG.
    ///
    /// A modifier is still offered and still worth keeping, because this file is parsed after the
    /// game's own map and a record created here sits at the head of the lookup list and BEATS an
    /// existing binding on the same key - see the note on the options page.
    /// </summary>
    public static readonly Binding[] All =
    {
        new(
            GameHudCommand,
            "Hide the game HUD",
            "KEY_F10",
            "NONE",
            new[]
            {
                "Hides the command bar, its build cameos and money, the minimap and the radar panel,",
                "leaving a clean view of the world. Press again to put it back exactly as it was.",
                "F10 is free in the retail key map - it is used only by the debug-build command map.",
            }),
        new(
            Command,
            "Hide the spectator HUD",
            "KEY_F11",
            "NONE",
            new[]
            {
                "Hides the readouts this project adds - ping, resends, frame rate, logic rate, the",
                "game clock and the per-player cash table. Independent of the key above, so you can",
                "have a clean world with just the numbers over it, or either one on its own.",
            }),
    };

    /// <summary>
    /// A name nothing else will ever ship. The directory is shared with any mod that drops keymap
    /// overrides there, and the updater's only way to disturb this file would be to deliver a file
    /// of the same name.
    /// </summary>
    public const string FileName = "GXUserKeys.ini";

    public static string DirectoryOf(string installDir) =>
        Path.Combine(installDir, "Data", "INI", "CommandMap");

    public static string PathOf(string installDir) =>
        Path.Combine(DirectoryOf(installDir), FileName);

    /// <summary>
    /// Key names exactly as KeyNames[] spells them (MetaEvent.h:166-262). The order is the order
    /// they are offered in, which is by usefulness rather than by the header's layout.
    /// </summary>
    public static readonly string[] Keys =
    {
        "KEY_F1", "KEY_F2", "KEY_F3", "KEY_F4", "KEY_F5", "KEY_F6",
        "KEY_F7", "KEY_F8", "KEY_F9", "KEY_F10", "KEY_F11", "KEY_F12",
        "KEY_A", "KEY_B", "KEY_C", "KEY_D", "KEY_E", "KEY_F", "KEY_G", "KEY_H", "KEY_I",
        "KEY_J", "KEY_K", "KEY_L", "KEY_M", "KEY_N", "KEY_O", "KEY_P", "KEY_Q", "KEY_R",
        "KEY_S", "KEY_T", "KEY_U", "KEY_V", "KEY_W", "KEY_X", "KEY_Y", "KEY_Z",
        "KEY_0", "KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6", "KEY_7", "KEY_8", "KEY_9",
        "KEY_MINUS", "KEY_EQUAL", "KEY_LBRACKET", "KEY_RBRACKET", "KEY_SEMICOLON",
        "KEY_APOSTROPHE", "KEY_TICK", "KEY_BACKSLASH", "KEY_COMMA", "KEY_PERIOD", "KEY_SLASH",
        "KEY_SPACE", "KEY_TAB", "KEY_ENTER", "KEY_BACKSPACE",
        "KEY_INS", "KEY_DEL", "KEY_HOME", "KEY_END", "KEY_PGUP", "KEY_PGDN",
        "KEY_UP", "KEY_DOWN", "KEY_LEFT", "KEY_RIGHT",
        "KEY_KP0", "KEY_KP1", "KEY_KP2", "KEY_KP3", "KEY_KP4", "KEY_KP5",
        "KEY_KP6", "KEY_KP7", "KEY_KP8", "KEY_KP9",
        "KEY_KPDEL", "KEY_KPSTAR", "KEY_KPMINUS", "KEY_KPPLUS", "KEY_KPENTER", "KEY_KPSLASH",
    };

    /// <summary>ModifierNames[] verbatim, minus NONE which the UI spells as "(none)".</summary>
    public static readonly string[] Modifiers =
    {
        "NONE", "CTRL", "ALT", "SHIFT", "CTRL_ALT", "SHIFT_CTRL", "SHIFT_ALT", "SHIFT_ALT_CTRL",
    };

    /// <summary>
    /// The file's whole contents.
    ///
    /// UseableIn IS NOT OPTIONAL AND ITS ABSENCE IS SILENT. TOGGLE_HUD_OVERLAY has no entry in
    /// MetaMap::generateMetaMap and none in the shipped CommandMap.ini, so this block is what
    /// CREATES its record - and a record created by getMetaMapRec starts at COMMANDUSABLE_NONE
    /// (MetaEvent.cpp:716). MetaEventTranslator skips every record that fails isMessageUsable
    /// (:576), so a block that sets only Key and Transition produces a key that does nothing at
    /// all, with no error anywhere. GAME and OBSERVER, not SHELL: there is no HUD to hide in the
    /// menus, and the readouts it hides gate themselves anyway.
    ///
    /// TABS ARE FORBIDDEN in this engine's INI files - INI::readLine asserts on one
    /// (INI.cpp:582) - so the indentation below is spaces, and must stay spaces.
    ///
    /// Description and DisplayName are deliberately absent. They parse through
    /// INI::parseAndTranslateLabel, which resolves a label out of the string table, and inventing a
    /// label that no .csf contains would be a lookup failure in a file whose whole purpose is to be
    /// harmless.
    /// </summary>
    public static string Body(IReadOnlyDictionary<string, (string Key, string Modifiers)> bound)
    {
        var b = new StringBuilder();
        b.Append("; GeneralsX keyboard override, written by the launcher. Safe to delete.\n");
        b.Append("; It sets only the bindings named below. Every other key in the game is untouched -\n");
        b.Append("; the game's own CommandMap.ini is inside INIZH.big and is not modified by anything here.\n");
        b.Append("; Nothing in this file affects the simulation, so it cannot stop you joining a game.\n");

        // Emitted in All order rather than dictionary order, so the file is stable across saves and
        // a diff of it shows a changed binding rather than a reshuffle.
        foreach (var binding in All)
        {
            if (!bound.TryGetValue(binding.Command, out var v))
            {
                continue;
            }
            b.Append($"CommandMap {binding.Command}\n");
            b.Append($"  Key = {v.Key}\n");
            b.Append("  Transition = DOWN\n");
            b.Append($"  Modifiers = {v.Modifiers}\n");
            b.Append("  UseableIn = GAME OBSERVER\n");
            b.Append("  Category = MISC\n");
            b.Append("End\n");
        }
        return b.ToString();
    }

    /// <summary>
    /// Write the bindings. Creates the directory; the engine tolerates it being empty.
    ///
    /// An EMPTY set deletes the file rather than writing a header with no blocks, so that "nothing
    /// bound" is the same state on disk as it was before the launcher ever ran. See Remove.
    /// </summary>
    public static bool Write(string installDir, IReadOnlyDictionary<string, (string Key, string Modifiers)> bound)
    {
        if (bound.Count == 0)
        {
            return Remove(installDir);
        }
        try
        {
            Directory.CreateDirectory(DirectoryOf(installDir));
            // ASCII rather than UTF-8: the engine reads these with fgets into a char buffer and a
            // BOM would arrive as three junk bytes on the first line. Nothing here is non-ASCII.
            File.WriteAllText(PathOf(installDir), Body(bound), new UTF8Encoding(false));
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// Remove ALL of this launcher's bindings by deleting the file, which puts the game back to
    /// having no record for either command - exactly where it was before the launcher wrote one.
    /// Writing "KEY_NONE" instead would leave a record behind whose mods-only branch
    /// (MetaEvent.cpp:580-590) fires on a bare modifier press, which is not "unbound".
    ///
    /// Unbinding only ONE of the two is NOT this method: that is Write with the other command
    /// still in the set, which rewrites the file with one block instead of deleting it.
    /// </summary>
    public static bool Remove(string installDir)
    {
        try
        {
            var p = PathOf(installDir);
            if (File.Exists(p))
            {
                File.Delete(p);
            }
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// What the file currently binds, or null when there is no file. Read back rather than
    /// remembered in gxlauncher.ini, for the same reason the resolution is not mirrored: the file
    /// on disk is the truth and a remembered copy goes stale the moment somebody edits or deletes
    /// it by hand.
    /// </summary>
    public static Dictionary<string, (string Key, string Modifiers)> Read(string installDir)
    {
        var result = new Dictionary<string, (string Key, string Modifiers)>(StringComparer.Ordinal);
        try
        {
            var p = PathOf(installDir);
            if (!File.Exists(p))
            {
                return result;
            }

            // PARSED PER BLOCK, NOT SCANNED. The previous reader took the last Key and the last
            // Modifiers line anywhere in the file, which was correct while there could only be one
            // block and silently wrong the moment there are two - it would report the second
            // block's key for both commands. Blocks are delimited by "CommandMap <name>" .. "End",
            // exactly as the engine's own INI reader delimits them.
            string? command = null;
            string? key = null;
            string? mods = null;

            void Flush()
            {
                if (command is not null && key is not null)
                {
                    result[command] = (key, mods ?? "NONE");
                }
                command = null;
                key = null;
                mods = null;
            }

            foreach (var raw in File.ReadAllLines(p))
            {
                var line = raw.Split(';')[0].Trim();
                if (line.Length == 0)
                {
                    continue;
                }
                if (line.StartsWith("CommandMap", StringComparison.OrdinalIgnoreCase))
                {
                    // A new block starts: whatever the previous one had is complete.
                    Flush();
                    var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                    command = parts.Length > 1 ? parts[1] : null;
                }
                else if (line.Equals("End", StringComparison.OrdinalIgnoreCase))
                {
                    Flush();
                }
                else if (IniLine.IsKey(line, "Key"))
                {
                    key = IniLine.Value(line);
                }
                else if (IniLine.IsKey(line, "Modifiers"))
                {
                    mods = IniLine.Value(line);
                }
            }
            // A final block with no closing End still counts; a hand-edited file may lack it.
            Flush();
            return result;
        }
        catch (IOException)
        {
            return result;
        }
        catch (UnauthorizedAccessException)
        {
            return result;
        }
    }
}
