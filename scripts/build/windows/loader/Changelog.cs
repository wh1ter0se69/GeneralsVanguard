/* The release notes, fetched from the CDN and treated as hostile text.
 *
 * GeneralsX @feature Claude 13/08/2026
 *
 * WHERE THIS DOCUMENT LIVES, AND WHY IT IS NOT IN THE MANIFEST. The signed manifest is served two
 * ways: over HTTPS from the CDN, and over UDP by every relay - where RELAY_MANIFEST_MAX is 476
 * bytes and publish-bootstrap.sh makes that a hard gate, because a relay REFUSES an oversized
 * document and then serves no manifest at all, which reads as "updates stopped working" with
 * nothing in any log. A changelog that keeps history cannot fit in 476 bytes and must never be
 * allowed to compete for them. So it is a SEPARATE OBJECT beside the manifest in the same bucket:
 *
 *     https://dl.wh1ter0se.dev/gxchangelog.txt
 *
 * That is not a new idea in this project - gxsetup.txt is already exactly this, a second flat text
 * object published by the same script to the same host with the same read-back verification. Using
 * the same shape means no new host in the client's trust surface, no new transport, and no new
 * failure mode for an operator to learn.
 *
 * IT IS NOT SIGNED, AND THAT IS A DELIBERATE SCOPE DECISION RATHER THAN AN OMISSION. This document
 * DESCRIBES; it never DECIDES. Nothing here selects a relay, names a download, or changes a
 * setting - the Ed25519-signed manifest remains the only thing that can cause a byte to be
 * installed. The worst an attacker who can serve bytes for this key achieves is wrong words on a
 * screen, and they could already serve a stale-but-genuine signed manifest, which is strictly
 * worse. What follows therefore treats the text as untrusted INPUT rather than as authority:
 * bounded in size and count, structurally validated, stripped of anything that could misrepresent
 * itself in a text UI, and never used as a path, a URL, a command or a format string.
 *
 * OFFLINE IS A NORMAL STATE, NOT AN ERROR. A missing, unreachable, oversized or malformed document
 * degrades to "no changelog available" and nothing else. It never blocks Play, never blocks the
 * window, and never turns into a dialog.
 *
 * NO WinForms IN THIS FILE - same reason as UpdateStatus.cs: the parser is the part that must be
 * testable on a machine that cannot run the launcher.
 */

using System.Globalization;
using System.Text;

namespace GeneralsX.Loader;

/// <summary>One published release, newest-first in <see cref="Changelog.Releases"/>.</summary>
internal sealed record ChangelogEntry(ulong Ordinal, string Commit, string Date, string Title,
    IReadOnlyList<string> Notes);

internal sealed record Changelog(IReadOnlyList<ChangelogEntry> Releases)
{
    public static readonly Changelog Empty = new(Array.Empty<ChangelogEntry>());
    public bool Any => Releases.Count > 0;
}

internal static class ChangelogSource
{
    /// <summary>
    /// The published object. The HOST must match GX_BOOTSTRAP_URL in gxlaunch.c - one name is
    /// compiled into this project, exactly as one name is compiled into the updater, and
    /// publish-bootstrap.sh checks both against what it is about to upload.
    /// </summary>
    public const string Url = "https://dl.wh1ter0se.dev/gxchangelog.txt";

    /// <summary>The local copy, so a launcher with no network still shows the last one it saw.</summary>
    public const string CacheFile = "gxchangelog.txt";

    /// <summary>
    /// EVERY BOUND HERE IS A REFUSAL, NOT A TRUNCATION, except the per-line one.
    ///
    /// Truncating a document to fit would render a fragment of an entry as though it were the
    /// entry, and a half-read release note is a misleading release note. A document that does not
    /// fit is not shown at all. The per-line cap is different: a long line is a cosmetic problem,
    /// so it is clipped with an ellipsis and the entry survives.
    /// </summary>
    public const int MaxBytes = 64 * 1024;
    public const int MaxReleases = 200;
    public const int MaxNotesPerRelease = 40;
    public const int MaxLineChars = 400;

    private const string Magic = "gxchangelog 1";

    /// <summary>
    /// Parse the document. Returns <see cref="Changelog.Empty"/> for anything it does not fully
    /// understand - there is no partial-credit path, because a half-understood changelog is
    /// indistinguishable from a correct one on screen.
    ///
    /// Grammar, one record per line, fields separated by single spaces:
    ///
    ///     gxchangelog 1
    ///     release &lt;ordinal&gt; &lt;commit&gt; &lt;YYYY-MM-DD&gt;
    ///     title   &lt;one line&gt;
    ///     note    &lt;one line&gt;
    ///
    /// Blank lines and '#' comments are ignored. UNRECOGNISED KEYWORDS ARE IGNORED RATHER THAN
    /// REFUSED, which is the same forward-compatibility rule the manifest parser follows for fields
    /// beyond the seventh: adding a record type must not break a launcher that has not been rebuilt.
    /// </summary>
    internal static Changelog Parse(string text)
    {
        if (string.IsNullOrEmpty(text) || Encoding.UTF8.GetByteCount(text) > MaxBytes)
        {
            return Changelog.Empty;
        }

        var releases = new List<ChangelogEntry>();
        ulong ordinal = 0;
        string commit = "", date = "", title = "";
        List<string> notes = new();
        var open = false;
        var seenMagic = false;

        void Close()
        {
            if (!open) return;
            releases.Add(new ChangelogEntry(ordinal, commit, date,
                title.Length > 0 ? title : "(no title)", notes));
            open = false;
            notes = new List<string>();
            title = "";
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
                // THE MAGIC LINE IS MANDATORY AND MUST BE FIRST. Without it, any text file served
                // from that key - an error page, a directory listing, a truncated upload - would be
                // parsed as "zero releases" and be indistinguishable from a real empty changelog.
                if (!string.Equals(line, Magic, StringComparison.Ordinal))
                {
                    return Changelog.Empty;
                }
                seenMagic = true;
                continue;
            }

            var sp = line.IndexOf(' ');
            var keyword = sp < 0 ? line : line[..sp];
            var rest = sp < 0 ? "" : line[(sp + 1)..].Trim();

            switch (keyword)
            {
                case "release":
                {
                    Close();
                    if (releases.Count >= MaxReleases)
                    {
                        return new Changelog(releases);
                    }
                    var p = rest.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                    if (p.Length != 3 ||
                        !ulong.TryParse(p[0], NumberStyles.None, CultureInfo.InvariantCulture, out ordinal) ||
                        !IsHex(p[1]) || !IsIsoDate(p[2]))
                    {
                        // A malformed header would otherwise silently attach the next release's
                        // notes to the previous entry.
                        return Changelog.Empty;
                    }
                    commit = p[1];
                    date = p[2];
                    open = true;
                    break;
                }

                // A title or note before any release header has nowhere to go. Ignored rather than
                // refused: it is inert, and refusing would make a stray leading line fatal.
                case "title":
                    if (open && title.Length == 0) title = Clip(rest);
                    break;

                case "note":
                    if (open && notes.Count < MaxNotesPerRelease && rest.Length > 0) notes.Add(Clip(rest));
                    break;
            }
        }
        Close();

        // NEWEST FIRST, and sorted here rather than trusted from the document. The generator writes
        // it in order, but "the file was in the right order" is not something a reader should
        // depend on when the file is fetched from the network.
        releases.Sort((a, b) => b.Ordinal.CompareTo(a.Ordinal));
        return new Changelog(releases);
    }

    private static bool IsHex(string s) =>
        s.Length is >= 7 and <= 40 && s.All(Uri.IsHexDigit);

    /// <summary>YYYY-MM-DD, and it must be a real date - "2026-13-45" is not one.</summary>
    private static bool IsIsoDate(string s) =>
        s.Length == 10 && DateOnly.TryParseExact(s, "yyyy-MM-dd", CultureInfo.InvariantCulture,
            DateTimeStyles.None, out _);

    /// <summary>
    /// Make one line of fetched text safe to put in a control.
    ///
    /// The interesting cases are not the obvious ones:
    ///   - C0 controls and DEL would let a document redraw or scramble the pane it is shown in;
    ///   - U+2028/U+2029 are line breaks to .NET's text rendering but not to String.Split('\n'),
    ///     so they smuggle a visual newline past a per-line cap;
    ///   - the bidi overrides (U+202A-U+202E, U+2066-U+2069) reorder rendered text, which is how a
    ///     line can read as something other than its own bytes - the one genuine spoofing vector in
    ///     a plain-text UI.
    /// Tab becomes a space rather than being dropped, so words do not run together.
    ///
    /// '&amp;' NEEDS NO ESCAPING HERE, but it does need UseMnemonic=false on every control that
    /// shows this text, which is the WinForms equivalent of markup injection: a label renders
    /// "R&amp;D" as "RD" with a mnemonic underline. ChangelogPanel sets it on every control it makes.
    /// </summary>
    private static string Clean(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var ch in s)
        {
            if (ch == '\t')
            {
                sb.Append(' ');
                continue;
            }
            if (char.IsControl(ch) ||
                // Spelled as escapes: every one of these is invisible in an editor, so a
                // literal here would be a line nobody could review.
                ch == '\u2028' || ch == '\u2029' ||               // line / paragraph separator
                (ch >= '\u202A' && ch <= '\u202E') ||             // LRE RLE PDF LRO RLO
                (ch >= '\u2066' && ch <= '\u2069'))               // LRI RLI FSI PDI
            {
                continue;
            }
            sb.Append(ch);
        }
        return sb.ToString().Trim();
    }

    private static string Clip(string s) =>
        s.Length <= MaxLineChars ? s : s[..MaxLineChars] + "…";

    /// <summary>
    /// Fetch the document, falling back to the cached copy. Never throws, never blocks a launch.
    ///
    /// Reads ONE BYTE MORE than the cap so that an oversized document is detected as oversized
    /// rather than silently truncated to exactly the cap - a truncation would be parsed, and the
    /// last entry in it would be a fragment presented as a release note.
    ///
    /// A fetch that succeeds but does not PARSE is not cached: overwriting a good cached copy with
    /// an error page would turn a transient CDN fault into a permanently empty changelog.
    /// </summary>
    public static async Task<Changelog> LoadAsync(string installDir, CancellationToken ct = default)
    {
        var cache = Path.Combine(installDir, CacheFile);

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            using var resp = await http.GetAsync(Url, HttpCompletionOption.ResponseHeadersRead, ct)
                .ConfigureAwait(false);
            if (resp.IsSuccessStatusCode)
            {
                using var stream = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                var buf = new byte[MaxBytes + 1];
                var got = 0;
                while (got < buf.Length)
                {
                    var n = await stream.ReadAsync(buf.AsMemory(got, buf.Length - got), ct)
                        .ConfigureAwait(false);
                    if (n == 0) break;
                    got += n;
                }
                if (got <= MaxBytes)
                {
                    var text = Encoding.UTF8.GetString(buf, 0, got);
                    var parsed = Parse(text);
                    if (parsed.Any)
                    {
                        try { File.WriteAllText(cache, text); } catch { /* cache is a nicety */ }
                        return parsed;
                    }
                }
            }
        }
        catch (Exception)
        {
            // Offline, DNS failure, TLS failure, timeout, cancelled. All the same answer: use what
            // we have on disk.
        }

        try
        {
            if (File.Exists(cache))
            {
                var info = new FileInfo(cache);
                if (info.Length <= MaxBytes)
                {
                    return Parse(File.ReadAllText(cache));
                }
            }
        }
        catch (Exception)
        {
            // An unreadable cache is not worth a message.
        }
        return Changelog.Empty;
    }
}
