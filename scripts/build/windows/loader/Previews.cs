/* In-game previews: a real screenshot of what an option actually does.
 *
 * GeneralsX @feature Claude 13/08/2026
 *
 * THE ASK, AND THE REASON IT IS A REAL SCREENSHOT. The owner's reference is GenPatcher, which shows
 * the chosen control bar rendered over a real map with a real base, real cameos and a real money
 * readout - not an icon and not a mockup. That is what makes it useful: you can see what you are
 * choosing before you choose it. Today these options are four filenames in a list, and nobody can
 * tell what "ControlBar Pro - 1440 layout" looks like without installing it and starting a match.
 *
 * WHY THE IMAGES ARE FETCHED AND NOT EMBEDDED. This program is a self-contained single-file exe
 * that rides the ~63.5 MB runtime payload tier, so every change to it costs every player that whole
 * tier. Embedding screenshots would mean replacing one picture costs 63.5 MB of everybody's
 * bandwidth, and adding a preview for a newly shipped archive would require a client release. The
 * project's founding rule is the opposite - "adding a relay must not require updating clients; new
 * relays are data, not a release" - and previews are data by exactly the same argument. So they
 * live beside the changelog on the CDN, and a new or corrected screenshot is a publish, not a
 * release.
 *
 * GeneralsX @refactor Claude 17/08/2026 THAT PARAGRAPH IS NO LONGER THE WHOLE STORY, and it is kept
 * verbatim above because the reasoning in it is still correct - it was outranked, not refuted.
 *
 * What it did not weigh is that a fetched picture is missing exactly when somebody is sitting in
 * front of the options screen with no connection, and "Preview unavailable offline" was the ONLY
 * sentence the preview boxes ever displayed on such a machine - five times over, once per option.
 * A feature whose normal state is an apology is not a feature. So the launcher now embeds its own
 * base screenshot and layers (PreviewArt.cs, previews/layers.txt) and composes ONE picture from
 * them, offline, always.
 *
 * THIS FILE IS NOW THE UPGRADE PATH RATHER THAN THE SOURCE, which is what keeps the founding rule
 * true: the document below can still supply a screenshot for an option the installed build has no
 * art for, so a preview for a newly shipped archive remains a publish rather than a release. What
 * it can no longer do is replace or contradict what shipped - see FillPreviewGapsAsync in
 * OptionsForm.cs for the two refusals that enforce it.
 *
 * STALENESS IS THE REAL RISK, NOT BANDWIDTH. A screenshot that no longer matches what the option
 * does is worse than no screenshot, because it is confidently wrong. Nothing about a picture makes
 * it announce that it has rotted. So every shot DECLARES THE ARTIFACT IT WAS CAPTURED FROM - the
 * content-named zip, "gxopt_33333333.zip", whose name changes whenever its bytes change. The
 * launcher compares that against what is actually installed and marks a preview whose artifact has
 * moved, rather than presenting it as current. The publisher side has the matching half: a gate
 * that lists previews whose tier was rebuilt without a new capture (see gx-previews.py).
 *
 * THE DOCUMENT IS UNTRUSTED, AND SO ARE THE IMAGES - more sharply than the changelog, because a
 * filename here becomes a PATH and the bytes reach an image decoder. Both are handled below:
 * filenames are validated as bare leaf names against a strict character class, and image bytes are
 * checked for a JPEG/PNG magic and against the sha256 the document declares before anything decodes
 * them. See LooksLikeImage for what that does and does not buy.
 *
 * NO WinForms IN THIS FILE - same rule as UpdateStatus.cs and Changelog.cs, so the parser and the
 * safety checks can be run on a machine that cannot run the launcher.
 */

using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace GeneralsX.Loader;

/// <summary>One screenshot: a leaf filename, its size on the wire, and what it depicts.</summary>
internal sealed record PreviewShot(string File, int Width, int Height, string Sha256, long Bytes,
    string Artifact);

/// <summary>
/// One previewable option. <see cref="Key"/> is the identity the launcher already uses: the overlay
/// archive's filename for game content ("340_ControlBarProZH.big"), or a "hud." pseudo-key for a
/// setting that has no file.
/// </summary>
internal sealed record PreviewOption(string Key, string Title, IReadOnlyList<string> Bullets,
    IReadOnlyList<PreviewShot> Shots);

internal sealed record PreviewSet(string Base, IReadOnlyList<PreviewOption> Options)
{
    public static readonly PreviewSet Empty = new("", Array.Empty<PreviewOption>());
    public bool Any => Options.Count > 0;

    public PreviewOption? For(string key) =>
        Options.FirstOrDefault(o => string.Equals(o.Key, key, StringComparison.OrdinalIgnoreCase));
}

internal static class PreviewSource
{
    public const string Url = "https://dl.wh1ter0se.dev/gxpreviews.txt";

    /// <summary>Where the cached images live, under the install directory.</summary>
    public const string CacheDir = "gxpreview";

    public const int MaxDocumentBytes = 64 * 1024;
    public const int MaxOptions = 100;
    public const int MaxShotsPerOption = 8;
    public const int MaxBulletsPerOption = 6;
    public const int MaxLineChars = 200;

    /// <summary>
    /// Two megabytes is generous for a 2560x1440 JPEG and mean for anything that is not one. The cap
    /// is enforced on the DECLARED size before a byte is requested, and again on what actually
    /// arrives - a server is free to ignore the first.
    /// </summary>
    public const long MaxImageBytes = 2 * 1024 * 1024;

    /// <summary>
    /// A ceiling on decoded pixels, checked from the document's own dimensions before decoding. It
    /// exists because the memory an image costs is a function of its DIMENSIONS, not of its
    /// compressed size - a "decompression bomb" is a few kilobytes on the wire and gigabytes in RAM.
    /// </summary>
    public const long MaxPixels = 4096L * 2304L;

    private const string Magic = "gxpreviews 1";

    /// <summary>
    /// A FILENAME FROM THIS DOCUMENT BECOMES A PATH, so it is validated as a bare leaf name and
    /// nothing else: letters, digits, dot, dash, underscore, and it must end .jpg or .png.
    ///
    /// That rejects every traversal shape at once rather than trying to spot them - "..\..\foo",
    /// "/etc/x", "C:\x", an alternate data stream "a.jpg:evil", a UNC "\\host\share" - because none
    /// of them survive the character class. A leading dot is refused separately so that "." and ".."
    /// cannot appear even though their characters are individually allowed.
    /// </summary>
    internal static bool IsSafeLeafName(string name)
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
            var ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// Is this plausibly a JPEG or a PNG, by its own first bytes?
    ///
    /// WHAT THIS BUYS AND WHAT IT DOES NOT. It does not make GDI+ safe on hostile input; nothing
    /// short of not decoding does that. What it removes is the whole class where a file named .jpg
    /// is handed to a DIFFERENT decoder inside GDI+ - the metafile formats (EMF/WMF) are the
    /// historically dangerous ones and are reached by content sniffing, not by extension. Combined
    /// with the sha256 check against the document, the only bytes that ever reach a decoder are
    /// bytes the publisher named.
    /// </summary>
    internal static bool LooksLikeImage(byte[] data)
    {
        if (data.Length < 8)
        {
            return false;
        }
        // JPEG: FF D8 FF.  PNG: 89 50 4E 47 0D 0A 1A 0A.
        if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        {
            return true;
        }
        return data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
               data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
    }

    internal static string Sha256Hex(byte[] data) =>
        Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();

    /// <summary>
    /// Parse the document.
    ///
    ///     gxpreviews 1
    ///     base https://dl.wh1ter0se.dev/gxpreview/
    ///     option 340_ControlBarProZH.big
    ///     title ControlBar Pro
    ///     bullet Wide-screen compatible with correctly scaled font sizes.
    ///     shot cbpro-2560.jpg 2560x1440 &lt;sha256&gt; 184320 gxopt_33333333.zip
    ///
    /// Same grammar family as gxchangelog.txt on purpose: one keyword-first line format across
    /// every document this project publishes, so there is one parser shape to review.
    ///
    /// A MALFORMED SHOT IS DROPPED, A MALFORMED OPTION HEADER IS FATAL. Dropping a bad shot loses
    /// one picture; mis-reading an option header would attach one option's screenshots to a
    /// different option, which is the "confidently wrong" failure this whole file exists to avoid.
    /// </summary>
    internal static PreviewSet Parse(string text)
    {
        if (string.IsNullOrEmpty(text) || Encoding.UTF8.GetByteCount(text) > MaxDocumentBytes)
        {
            return PreviewSet.Empty;
        }

        var options = new List<PreviewOption>();
        var baseUrl = "";
        string key = "", title = "";
        var bullets = new List<string>();
        var shots = new List<PreviewShot>();
        var open = false;
        var seenMagic = false;

        void Close()
        {
            if (!open) return;
            options.Add(new PreviewOption(key, title.Length > 0 ? title : key, bullets, shots));
            bullets = new List<string>();
            shots = new List<PreviewShot>();
            title = "";
            open = false;
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
                    return PreviewSet.Empty;
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
                    // HTTPS ONLY, and absolute. A document that could point the launcher at http://
                    // would downgrade every image fetch on the strength of an unsigned file.
                    if (rest.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                    {
                        baseUrl = rest.EndsWith('/') ? rest : rest + "/";
                    }
                    break;

                case "option":
                    Close();
                    if (options.Count >= MaxOptions)
                    {
                        return new PreviewSet(baseUrl, options);
                    }
                    // The key is matched against archive filenames and against our own "hud." names.
                    // Same character rules as a filename minus the extension requirement.
                    if (rest.Length is 0 or > 80 || rest.Any(c => c == '/' || c == '\\' || c == ' '))
                    {
                        return PreviewSet.Empty;
                    }
                    key = rest;
                    open = true;
                    break;

                case "title":
                    if (open && title.Length == 0) title = Clip(rest);
                    break;

                case "bullet":
                    if (open && bullets.Count < MaxBulletsPerOption && rest.Length > 0)
                        bullets.Add(Clip(rest));
                    break;

                case "shot":
                {
                    if (!open || shots.Count >= MaxShotsPerOption) break;
                    var p = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (p.Length != 5) break;
                    if (!IsSafeLeafName(p[0])) break;

                    var wh = p[1].Split('x');
                    if (wh.Length != 2 ||
                        !int.TryParse(wh[0], NumberStyles.None, CultureInfo.InvariantCulture, out var w) ||
                        !int.TryParse(wh[1], NumberStyles.None, CultureInfo.InvariantCulture, out var h) ||
                        w <= 0 || h <= 0 || (long)w * h > MaxPixels)
                    {
                        break;
                    }
                    if (p[2].Length != 64 || !p[2].All(Uri.IsHexDigit)) break;
                    if (!long.TryParse(p[3], NumberStyles.None, CultureInfo.InvariantCulture, out var bytes) ||
                        bytes <= 0 || bytes > MaxImageBytes)
                    {
                        break;
                    }
                    shots.Add(new PreviewShot(p[0], w, h, p[2].ToLowerInvariant(), bytes, p[4]));
                    break;
                }
            }
        }
        Close();
        return new PreviewSet(baseUrl, options);
    }

    /// <summary>
    /// Is this shot still describing what is installed?
    ///
    /// The artifact names are content-addressed - "gxopt_&lt;content8&gt;.zip" changes whenever the
    /// archives inside it change - so a shot whose artifact is not among the installed ones was
    /// captured from different bytes than the player has. It is still SHOWN, because an old picture
    /// of the right option is more useful than a grey box; it is labelled, because presenting it as
    /// current is the failure mode.
    ///
    /// UNKNOWN IS NOT STALE. With no installed-artifact list to compare against - a fresh install,
    /// an unreadable state file - this returns true rather than marking everything out of date,
    /// which would train people to ignore the label.
    /// </summary>
    internal static bool ShotMatchesInstall(PreviewShot shot, IReadOnlyCollection<string> installedArtifacts) =>
        ArtifactMatchesInstall(shot.Artifact, installedArtifacts);

    /// <summary>
    /// The same test, for a picture that is not a fetched shot.
    ///
    /// GeneralsX @refactor Claude 17/08/2026 Split out because the EMBEDDED base screenshot needs
    /// exactly this check and has no PreviewShot to carry it. Cross-model review caught that
    /// embedding the art had quietly dropped the staleness half of the design: a fetched shot was
    /// checked against gxinstalled.txt and labelled when its tier had moved, and an embedded one was
    /// not checked at all - so a rebuilt optional tier would leave the launcher asserting that a
    /// screenshot of older bytes is what the player has. The whole point of naming the artifact was
    /// that a picture cannot announce its own rot.
    /// </summary>
    internal static bool ArtifactMatchesInstall(string artifact, IReadOnlyCollection<string> installedArtifacts)
    {
        if (installedArtifacts.Count == 0 || artifact.Length == 0)
        {
            return true;
        }
        return installedArtifacts.Contains(artifact, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>The artifact names this install currently has, from gxinstalled.txt.</summary>
    internal static HashSet<string> InstalledArtifacts(IEnumerable<string> installedLines)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in installedLines)
        {
            var l = raw.Trim();
            if (!l.StartsWith("item ", StringComparison.OrdinalIgnoreCase)) continue;
            var p = l.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (p.Length >= 4) set.Add(p[2]);
        }
        return set;
    }

    private static string Clean(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var ch in s)
        {
            if (ch == '\t') { sb.Append(' '); continue; }
            // Same class as Changelog.Clean: C0/C1 controls, the Unicode line separators and the
            // bidi overrides. Spelled as escapes because every one of them is invisible in an editor.
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

    private static string Clip(string s) => s.Length <= MaxLineChars ? s : s[..MaxLineChars] + "…";

    /// <summary>
    /// Fetch the document, falling back to the cached copy. Same posture as the changelog: never
    /// throws, never blocks, and a failure is an empty set rather than an error anybody sees.
    /// </summary>
    public static async Task<PreviewSet> LoadAsync(string installDir, CancellationToken ct = default)
    {
        var cacheDir = Path.Combine(installDir, CacheDir);
        var cache = Path.Combine(cacheDir, "gxpreviews.txt");

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            var text = await http.GetStringAsync(Url, ct).ConfigureAwait(false);
            if (Encoding.UTF8.GetByteCount(text) <= MaxDocumentBytes)
            {
                var parsed = Parse(text);
                if (parsed.Any)
                {
                    try
                    {
                        Directory.CreateDirectory(cacheDir);
                        File.WriteAllText(cache, text);
                    }
                    catch { /* the cache is a nicety, not a requirement */ }
                    return parsed;
                }
            }
        }
        catch (Exception)
        {
            // Offline is a normal state for this feature.
        }

        try
        {
            if (File.Exists(cache) && new FileInfo(cache).Length <= MaxDocumentBytes)
            {
                return Parse(File.ReadAllText(cache));
            }
        }
        catch (Exception) { }
        return PreviewSet.Empty;
    }

    /// <summary>
    /// Get one screenshot's bytes, from the cache or the CDN, or null.
    ///
    /// EVERY GATE IS APPLIED TO WHAT ARRIVED, not to what was promised: the declared size is used to
    /// refuse a fetch cheaply, and the real bytes are then capped, magic-checked and hashed against
    /// the document before they are written to disk or handed to a decoder. A cached file is
    /// re-hashed on read for the same reason the updater re-verifies its cached manifest - a file
    /// beside the executable is not more trustworthy than the network, it is only closer.
    /// </summary>
    public static async Task<byte[]?> LoadShotAsync(string installDir, PreviewSet set, PreviewShot shot,
        CancellationToken ct = default)
    {
        if (!IsSafeLeafName(shot.File) || shot.Bytes > MaxImageBytes || set.Base.Length == 0)
        {
            return null;
        }

        var cacheDir = Path.Combine(installDir, CacheDir);
        var path = Path.Combine(cacheDir, shot.File);

        try
        {
            if (File.Exists(path) && new FileInfo(path).Length == shot.Bytes)
            {
                var have = await File.ReadAllBytesAsync(path, ct).ConfigureAwait(false);
                if (Sha256Hex(have) == shot.Sha256 && LooksLikeImage(have))
                {
                    return have;
                }
            }
        }
        catch (Exception) { }

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
            using var resp = await http.GetAsync(set.Base + shot.File, ct).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                return null;
            }
            using var stream = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            using var ms = new MemoryStream();
            var buf = new byte[64 * 1024];
            int n;
            while ((n = await stream.ReadAsync(buf, ct).ConfigureAwait(false)) > 0)
            {
                ms.Write(buf, 0, n);
                if (ms.Length > MaxImageBytes)
                {
                    return null;        // a server that ignores the declared size
                }
            }
            var data = ms.ToArray();
            if (data.Length != shot.Bytes || Sha256Hex(data) != shot.Sha256 || !LooksLikeImage(data))
            {
                return null;
            }
            try
            {
                Directory.CreateDirectory(cacheDir);
                await File.WriteAllBytesAsync(path, data, ct).ConfigureAwait(false);
            }
            catch { /* an unwritable cache costs the next launch a re-fetch, nothing more */ }
            return data;
        }
        catch (Exception)
        {
            return null;
        }
    }
}
