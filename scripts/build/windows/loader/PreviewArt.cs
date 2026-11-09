/* ONE picture of the game, assembled from a base screenshot plus one layer per option.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * THE ASK, VERBATIM: "one picture that changes dynamically for all", and "enabling/disabling
 * features shows on that same picture not on 12839172381 different stuff". What shipped before this
 * was the opposite - one screenshot at the top of the page and then a SEPARATE framed preview box
 * under every optional archive, five of them, every one reading "Preview unavailable offline."
 *
 * WHY A BASE PLUS LAYERS AND NOT ONE PICTURE PER COMBINATION. Five toggles is 32 combinations, and
 * the sixth option would make it 64. Nobody captures 32 screenshots, and nobody re-captures them
 * when one archive changes - so a pre-rendered set is a set that rots. A base plus one layer per
 * option is linear: a new option is one new file, and a corrected option is one corrected file.
 *
 * THE ORDER LAYERS ARE PAINTED IN IS THE ENGINE'S, NOT A CHOICE MADE HERE. If two archives both
 * supply the art in the same place, the game resolves it by FILENAME, first match wins - that is
 * what the community's numeric prefixes ("340_") are for, and it is implemented at
 * StdBIGFileSystem.cpp:988-991 / Win32BIGFileSystem.cpp:1038-1041, which walk the already-mounted
 * range with comparePathNoCase(existing, ours) <= 0 and insert at the first name that sorts after
 * ours. Lookups then take instance 0 - the first entry of the equal_range
 * (ArchiveFileSystem.cpp:317) - so the archive whose basename sorts EARLIEST wins the file.
 *
 * Painting is the other way round: the winner must go on TOP, so layers are drawn in DESCENDING
 * filename order and the earliest-sorting name lands last. See MountOrder below, and note what is
 * NOT consulted: the order the launcher stages the files in. place_tree walks the staging directory
 * with FindFirstFileW (gxlaunch.c:1830-1858) in whatever order the filesystem hands back, and the
 * engine's insertion is by explicit filename comparison rather than by walk order - so staging
 * order cannot affect the game and must not affect the picture either.
 *
 * WHY THE ART IS EMBEDDED RATHER THAN FETCHED, which reverses what Previews.cs used to argue. That
 * file's header still records the case for fetching (a picture is data, and data should not need a
 * client release) and the case was not wrong - it was just outranked. Fetched art is unavailable
 * offline, and "Preview unavailable offline" was the ONLY thing five of five preview boxes ever
 * said. A feature whose normal state is an apology is not a feature. So the launcher now ships its
 * own art inside the single-file exe, where it cannot go missing, and the fetched document survives
 * as a strictly ADDITIVE upgrade: it may fill in a layer for an option this build has no art for -
 * which is what keeps "a preview for a newly shipped archive needs no client release" true - and it
 * may never replace or contradict what is embedded. See PreviewGapFill in OptionsForm.cs.
 *
 * HONESTY IS THE WHOLE POINT AND IT IS ENFORCED BY THE MANIFEST, NOT BY GOOD INTENTIONS. A picture
 * that shows a state the player did not select is worse than no picture, because it is a promise
 * about what they are about to see. So the base declares WHICH OPTIONS IT WAS CAPTURED WITH, and an
 * option is only claimed to be depicted when the manifest says so - through 'shows' (the base
 * itself already shows it), 'layer' (there is art for it) or 'silent' (it provably changes nothing
 * in a picture of this kind, with the reason recorded). Everything else is reported to the player as
 * "no preview available", and the caption names it. An option with no evidence gets no claim.
 *
 * NO WinForms AND NO GDI+ IN THIS FILE - same rule as Previews.cs, Changelog.cs and UpdateStatus.cs:
 * the parser and the ordering can then be run and reasoned about on a machine that cannot run the
 * launcher, which is where this file was written.
 */

using System.Reflection;
using System.Text;

namespace GeneralsX.Loader;

/// <summary>
/// How the engine orders overlay archives, reproduced exactly.
///
/// strcasecmp, NOT .NET's OrdinalIgnoreCase, and the difference is not pedantry. The engine folds
/// with tolower (StdBIGFileSystem.cpp:299-310 - _stricmp on Windows, strcasecmp elsewhere, both
/// lower-folding for ASCII); OrdinalIgnoreCase folds with ToUpperInvariant. The two disagree
/// wherever a letter is compared against one of the six bytes between 'Z' and 'a' - and '_' is one
/// of them, in the middle of "340_ControlBarProZH.big". Today's five names happen not to hit it;
/// the next name added would, and the failure would be a picture whose layers are stacked in an
/// order the game does not use, which is invisible until it is wrong.
/// </summary>
internal static class MountOrder
{
    /// <summary>
    /// Negative when <paramref name="a"/> takes PRECEDENCE over <paramref name="b"/> - it sorts
    /// earlier, so it is the one the engine's instance-0 lookup finds.
    /// </summary>
    public static int Compare(string a, string b)
    {
        var n = Math.Min(a.Length, b.Length);
        for (var i = 0; i < n; i++)
        {
            // ToLowerInvariant per char rather than on the whole string: same fold, no allocation,
            // and it cannot be affected by a culture that lowercases a pair of characters as one.
            var ca = char.ToLowerInvariant(a[i]);
            var cb = char.ToLowerInvariant(b[i]);
            if (ca != cb)
            {
                return ca < cb ? -1 : 1;
            }
        }
        return a.Length.CompareTo(b.Length);
    }

    /// <summary>
    /// The order layers must be PAINTED in: lowest precedence first, so the archive the engine
    /// would let win is drawn last and ends up on top. Descending mount order, in other words.
    /// </summary>
    public static List<string> PaintOrder(IEnumerable<string> names)
    {
        var list = new List<string>(names);
        list.Sort((x, y) => Compare(y, x));
        return list;
    }
}

/// <summary>Why an option is or is not depicted in the composed picture.</summary>
internal enum PreviewDepiction
{
    /// <summary>No art and no claim. The player is told so, in the row and in the caption.</summary>
    None,

    /// <summary>The base screenshot was itself captured with this option enabled.</summary>
    Base,

    /// <summary>There is a layer for it, so turning it on adds that layer to the picture.</summary>
    Layer,

    /// <summary>
    /// It provably changes nothing a picture of this kind can show, and the manifest records why -
    /// e.g. !GXUI.big contains exactly one member, Window\Menus\PopupHostGame.wnd, so it cannot
    /// alter an in-game shot. A claim, with its evidence attached.
    /// </summary>
    Silent,
}

/// <summary>One option's art, or the reasoned absence of it.</summary>
internal sealed record PreviewLayer(string Key, PreviewDepiction How, byte[]? Bytes, string Why);

/// <summary>
/// The art this launcher can draw, and what it is honestly allowed to claim about it.
///
/// <see cref="Base"/> null means no art shipped at all, which is a legitimate state - the picture
/// then says so plainly instead of blaming the network.
/// </summary>
internal sealed class PreviewArt
{
    public byte[]? Base { get; init; }

    /// <summary>What the base picture is, in one line, for the caption under it.</summary>
    public string BaseCaption { get; init; } = "";

    /// <summary>
    /// The content-addressed artifact the base was captured from - "gxopt_&lt;content8&gt;.zip",
    /// whose name changes the moment the archives inside it change.
    ///
    /// This is the ONLY thing that can tell a player the picture has rotted. The optional-content
    /// tier and this launcher ship on separate tiers and update independently, so a rebuilt tier
    /// leaves a screenshot of bytes nobody has any more - and nothing about a picture announces
    /// that. Compared against gxinstalled.txt; empty means the manifest did not say, which is
    /// treated as "cannot tell" rather than as "stale", for the same reason
    /// PreviewSource.ShotMatchesInstall treats an unknown install as current: marking everything out
    /// of date trains people to ignore the label.
    /// </summary>
    public string Artifact { get; init; } = "";

    private readonly Dictionary<string, PreviewLayer> _layers =
        new(StringComparer.OrdinalIgnoreCase);

    public static readonly PreviewArt Empty = new();

    public bool Any => Base is not null;

    public PreviewLayer For(string key) =>
        _layers.TryGetValue(key, out var l) ? l : new PreviewLayer(key, PreviewDepiction.None, null, "");

    /// <summary>
    /// Add a layer, refusing to overwrite one that is already there.
    ///
    /// FIRST WRITER WINS, and that is what makes the fetched document unable to contradict the
    /// embedded art: the embedded manifest is parsed first, so a document that names a key we
    /// already have art or a claim for is ignored for that key rather than trusted over it.
    /// </summary>
    public bool Add(PreviewLayer layer)
    {
        if (layer.Key.Length == 0 || _layers.ContainsKey(layer.Key))
        {
            return false;
        }
        _layers[layer.Key] = layer;
        return true;
    }

    public bool Has(string key) => _layers.ContainsKey(key);
}

/// <summary>
/// The embedded art set, and the manifest that says what it is.
///
/// EVERY NAME IS DATA. Nothing in this file spells an archive filename, a label or a default -
/// exactly as OptionsPanel refuses to compile in the optional list. The manifest maps an archive
/// name to a layer file, and the layer file is looked up by that name; adding art for a new option
/// is a new .png and a new manifest line, with no C# change and therefore no chance of the two
/// disagreeing.
/// </summary>
internal static class PreviewArtSource
{
    /// <summary>
    /// The resource-name prefix. Set explicitly by LogicalName in the .csproj rather than left to
    /// MSBuild's default naming, because the default mangles the characters these files actually
    /// contain - a leading '!' and several dots - and a resource whose name is not what the code
    /// asks for is a picture that silently never appears.
    /// </summary>
    private const string Prefix = "gxpreview/";

    private const string ManifestName = "layers.txt";
    private const string Magic = "gxlayers 1";

    /// <summary>Generous for a manifest of a dozen lines and mean for anything that is not one.</summary>
    private const int MaxManifestBytes = 16 * 1024;

    private const int MaxLayers = 64;
    private const int MaxWhyChars = 160;

    private static PreviewArt? s_cache;

    /// <summary>
    /// Read the embedded manifest and every layer it names. Cached: the bytes are in this process's
    /// own image, but decoding the manifest on every page build would still be waste, and the
    /// OptionsPanel is rebuilt on every click of a sidebar category.
    /// </summary>
    public static PreviewArt Embedded()
    {
        return s_cache ??= Load();
    }

    private static PreviewArt Load()
    {
        try
        {
            var manifest = Resource(ManifestName);
            if (manifest is null || manifest.Length > MaxManifestBytes)
            {
                return PreviewArt.Empty;
            }
            return Parse(Encoding.UTF8.GetString(manifest), Resource);
        }
        catch (Exception)
        {
            // A launcher that cannot read its own resources must still open its options screen.
            return PreviewArt.Empty;
        }
    }

    /// <summary>
    /// Parse the manifest.
    ///
    ///     gxlayers 1
    ///     base cbpro-1600x840.jpg
    ///     caption In game on a desert map, captured with ControlBar Pro enabled.
    ///     shows 340_ControlBarProZH.big
    ///     layer !HotkeysLeikezeZH.big hotkeys.png
    ///     silent !GXUI.big It changes the Host Game screen, not the in-game view.
    ///
    /// Same keyword-first grammar as gxchangelog.txt and gxpreviews.txt, so there is one document
    /// shape to review across this project rather than three.
    ///
    /// <paramref name="fetch"/> is how a named file is turned into bytes, passed in so the parser
    /// can be exercised without an assembly to read from.
    ///
    /// A LAYER WHOSE FILE IS MISSING BECOMES "no preview available", NOT an error and not a silent
    /// success. The manifest is ours, so a mismatch is a packaging mistake rather than hostile
    /// input - but the player-visible consequence of getting it wrong must be an honest absence.
    /// </summary>
    internal static PreviewArt Parse(string text, Func<string, byte[]?> fetch)
    {
        if (string.IsNullOrEmpty(text))
        {
            return PreviewArt.Empty;
        }

        byte[]? baseBytes = null;
        var caption = "";
        var artifact = "";
        var pending = new List<PreviewLayer>();
        var seenMagic = false;

        // One depiction per key, refused where the claim is made rather than where it is stored.
        //
        // Everything downstream reads a key's depiction as a statement to the player - "the picture
        // above shows this switched off", "this layer is drawn on top", "a picture like this one
        // cannot show it". Those contradict each other, so a key carrying two of them makes one a
        // lie. PreviewArt.Add is the authority and already drops the loser, but only after 'layer'
        // has fetched art that will be thrown away, and only somewhere a reader of this loop cannot
        // see. Same verdict, stated where the contradiction is written.
        //
        // OrdinalIgnoreCase deliberately, because that is the comparer Add's dictionary uses: a
        // guard that thought "Hotkeys" and "hotkeys" were different keys would pass a pair here
        // that Add then silently collapses, which is worse than not guarding at all.
        bool AlreadyClaimed(string k)
        {
            foreach (var p in pending)
            {
                if (string.Equals(p.Key, k, StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }
            return false;
        }

        foreach (var raw in text.Split('\n'))
        {
            var line = Clean(raw);
            if (line.Length == 0 || line[0] == '#')
            {
                continue;
            }
            if (!seenMagic)
            {
                if (!string.Equals(line, Magic, StringComparison.Ordinal))
                {
                    return PreviewArt.Empty;
                }
                seenMagic = true;
                continue;
            }

            var sp = line.IndexOf(' ');
            var keyword = sp < 0 ? line : line[..sp];
            var rest = sp < 0 ? "" : line[(sp + 1)..].Trim();

            switch (keyword)
            {
                case "base":
                    if (baseBytes is null && IsSafeArtName(rest))
                    {
                        baseBytes = fetch(rest);
                    }
                    break;

                case "caption":
                    if (caption.Length == 0)
                    {
                        caption = Clip(rest, MaxWhyChars);
                    }
                    break;

                case "artifact":
                    // Same shape rules as an option key: one token, no spaces or separators. It is
                    // only ever compared against the names in gxinstalled.txt.
                    if (artifact.Length == 0 && IsSafeKey(rest))
                    {
                        artifact = rest;
                    }
                    break;

                case "shows":
                    if (IsSafeKey(rest) && pending.Count < MaxLayers && !AlreadyClaimed(rest))
                    {
                        pending.Add(new PreviewLayer(rest, PreviewDepiction.Base, null, ""));
                    }
                    break;

                case "layer":
                {
                    var sp2 = rest.IndexOf(' ');
                    if (sp2 <= 0 || pending.Count >= MaxLayers)
                    {
                        break;
                    }
                    var key = rest[..sp2];
                    var file = rest[(sp2 + 1)..].Trim();
                    if (!IsSafeKey(key) || !IsSafeArtName(file) || AlreadyClaimed(key))
                    {
                        break;
                    }
                    var bytes = fetch(file);
                    // No bytes, no claim. Falling through to None is what stops a mistyped
                    // filename from being reported to the player as a depicted option.
                    if (bytes is not null)
                    {
                        pending.Add(new PreviewLayer(key, PreviewDepiction.Layer, bytes, ""));
                    }
                    break;
                }

                case "silent":
                {
                    var sp2 = rest.IndexOf(' ');
                    if (sp2 <= 0 || pending.Count >= MaxLayers)
                    {
                        break;
                    }
                    var key = rest[..sp2];
                    var why = rest[(sp2 + 1)..].Trim();
                    // A 'silent' claim with no stated reason is refused outright. The reason is the
                    // only thing that makes the claim checkable by whoever reads this next.
                    if (IsSafeKey(key) && why.Length > 0 && !AlreadyClaimed(key))
                    {
                        pending.Add(new PreviewLayer(key, PreviewDepiction.Silent, null,
                            Clip(why, MaxWhyChars)));
                    }
                    break;
                }
            }
        }

        // NO BASE MEANS NO CLAIMS AT ALL. Every depiction verdict above is relative to the base
        // picture - "the base already shows it", "this layer goes on top of it", "it changes
        // nothing in a picture like this one". With no base there is no picture, and carrying the
        // claims forward would mark options as depicted in an image that does not exist.
        if (baseBytes is null)
        {
            return PreviewArt.Empty;
        }

        var art = new PreviewArt { Base = baseBytes, BaseCaption = caption, Artifact = artifact };
        foreach (var l in pending)
        {
            art.Add(l);
        }
        return art;
    }

    /// <summary>
    /// A layer filename from the manifest becomes a RESOURCE NAME, not a path - so it cannot
    /// traverse anywhere - but it is still validated to the same shape gx-previews.py and
    /// PreviewSource enforce, so that one rule describes art everywhere in this project and a file
    /// that would be refused on the wire is refused here too.
    /// </summary>
    internal static bool IsSafeArtName(string name)
    {
        if (name.Length is 0 or > 64 || name[0] == '.')
        {
            return false;
        }
        if (!name.EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) &&
            !name.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        foreach (var ch in name)
        {
            // '!' is here because the art is named after the overlay archives it depicts, and
            // several of those lead with it (!GXUI.big, !HotkeysLeikezeZH.big) to sort first in the
            // engine's mount order. Without it this refused exactly the naming convention the
            // manifest tells the next author to follow, and refused it SILENTLY - the row just read
            // "no preview available" with nothing saying the line had been dropped.
            // It is not a path character: these names index an embedded resource, never the disk.
            var ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '!';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// An option key is an overlay archive's filename, so it carries the characters those names
    /// actually use - a leading '!' among them. It is never turned into a path or a resource name;
    /// it is only ever compared against the names optional.txt lists.
    /// </summary>
    internal static bool IsSafeKey(string key) =>
        key.Length is > 0 and <= 80 && !key.Contains('/') && !key.Contains('\\') && !key.Contains(' ');

    private static byte[]? Resource(string name)
    {
        try
        {
            using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(Prefix + name);
            if (s is null)
            {
                return null;
            }
            using var ms = new MemoryStream();
            s.CopyTo(ms);
            return ms.ToArray();
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>
    /// Same control-character class Changelog.Clean and PreviewSource.Clean strip. This document is
    /// ours rather than fetched, so this is hygiene rather than defence - but a bidi override
    /// pasted into a caption would reorder the sentence on screen, and there is no reason for one
    /// to survive here when it does not survive anywhere else.
    /// </summary>
    private static string Clean(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var ch in s)
        {
            if (ch == '\t') { sb.Append(' '); continue; }
            if (char.IsControl(ch) ||
                ch == '\u2028' || ch == '\u2029' ||
                (ch >= '\u202A' && ch <= '\u202E') ||
                (ch >= '\u2066' && ch <= '\u2069'))
            {
                continue;
            }
            sb.Append(ch);
        }
        return sb.ToString().Trim();
    }

    private static string Clip(string s, int max) => s.Length <= max ? s : s[..max] + "…";
}

/// <summary>
/// One line of GeneralsXPatch/optional.txt: an archive the launcher may toggle, the label to show,
/// and what the payload shipped it as.
///
/// <see cref="DefaultOn"/> is NULLABLE on purpose. A payload built before the default was carried
/// through this file simply does not say, and "we do not know" has to be distinguishable from "it
/// shipped off" - printing "Default: off" for an archive that shipped on is exactly the kind of
/// confidently-wrong statement the whole preview design is trying to avoid.
/// </summary>
internal sealed record OptionalEntry(string Name, string Label, bool? DefaultOn);

internal static class OptionalList
{
    /// <summary>
    /// Parse the manifest the packager stages beside the archives.
    ///
    ///     340_ControlBarProZH.big|ControlBar Pro|on
    ///
    /// THE THIRD FIELD IS NEW AND ITS ABSENCE IS NOT AN ERROR. The two-field form
    /// "&lt;name&gt;|&lt;label&gt;" is what every payload built before 17/08/2026 contains, and a
    /// launcher updates on its own tier - so a new launcher WILL meet an old payload, and it must
    /// show the toggles rather than refuse the section. It reports the default as unknown instead.
    ///
    /// The default is deliberately the LAST field rather than the first, even though the repo's
    /// overlay-optional.txt puts it first. Appending keeps the invariant an OLD launcher relies on:
    /// its Split('|', 2) still finds the right filename in field 0, so a new payload under an old
    /// launcher still toggles the right archive - it merely renders "ControlBar Pro|on" as the
    /// label. Leading the line with the default instead would make field 0 the word "on", the
    /// launcher would look for a file called "on", find neither name, and drop every toggle.
    /// </summary>
    public static List<OptionalEntry> Parse(IEnumerable<string> lines)
    {
        var entries = new List<OptionalEntry>();
        foreach (var raw in lines)
        {
            var line = raw.Split('#')[0].Trim();
            if (line.Length == 0)
            {
                continue;
            }
            var f = line.Split('|', 3);
            if (f.Length < 2 || f[0].Length == 0)
            {
                continue;
            }
            bool? dflt = null;
            if (f.Length == 3)
            {
                var d = f[2].Trim();
                if (string.Equals(d, "on", StringComparison.OrdinalIgnoreCase))
                {
                    dflt = true;
                }
                else if (string.Equals(d, "off", StringComparison.OrdinalIgnoreCase))
                {
                    dflt = false;
                }
                // Anything else stays null. A field we cannot read is not a default we can quote.
            }
            entries.Add(new OptionalEntry(f[0], f[1].Length > 0 ? f[1] : f[0], dflt));
        }
        return entries;
    }
}
