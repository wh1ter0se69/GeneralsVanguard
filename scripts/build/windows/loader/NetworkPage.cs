/* The NETWORK page's test: eight checks, a verdict in words, and one line to paste.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * WHAT THIS PAGE IS FOR, stated first because it decided everything else. Somebody could not join
 * their friend, they have looked at the game's error - which says something about a connection -
 * and they have come here. So the page is not a network report card. It is an answer to one
 * question, and the checks are ordered by how often each one is the real cause rather than by which
 * are most impressive to measure. The two most common causes are a stale build and a missing
 * archive; neither is a network fault and both are refused with a message that sounds like one.
 *
 * WHY THE VERDICT IS A SENTENCE AND NOT A SCORE. Eight coloured dots is a list, and a list makes
 * the reader do the diagnosis. The line above the rows names the FIRST thing worth fixing, in
 * plain words, in the order it should be fixed. The dots are the evidence for it, not the product.
 * That is the one deliberate departure from every other launcher's network page, and it is the
 * whole reason this one is worth having.
 *
 * THE DECISIONS ARE NOT IN THIS FILE. Everything that decides what a result MEANS lives in
 * NetworkProbe.cs, which references no WinForms and no Win32 and is compiled and RUN by
 * ../loader-tests on a Mac. This file positions controls and paints them. That split is not
 * tidiness: the launcher has already shipped a wrong verdict once, and the reason it could was that
 * the logic could only be executed by installing a release on Windows and pressing a button.
 *
 * IT SURVIVES A TAB SWITCH. Select() clears and rebuilds the whole pane, so anything held only in a
 * control is lost the moment somebody looks at GRAPHICS. The results live on the panel and the page
 * re-renders them, because a diagnostic you have to re-run to re-read is a diagnostic people
 * screenshot before they have finished reading it.
 */

using System.Globalization;

namespace GeneralsX.Loader;

/// <summary>
/// How a status looks. ONE definition, used by every control that paints one.
///
/// THE TWO NEW COLOURS ARE DERIVED FROM THE ACCENT, NOT PICKED - the same construction, and for the
/// same reason, as LauncherTheme.Caution. Accent is HSL(211.5deg, 80.7%, 57.5%); Caution is that
/// hue rotated to 45deg with saturation and lightness untouched, so it carries identical visual
/// weight on this ground and needs no separate tuning. Ok and Bad are the same rotation taken
/// further round:
///
///     Accent   211.5deg   #3B8EEA
///     Ok       145deg     #3BEA84
///     Caution   45deg     #EABE3B     (already in the palette)
///     Bad        0deg     #EA3B3B
///
/// Choosing a red and a green by eye is how a palette acquires two colours brighter than everything
/// around them that drag the eye off the row that matters; deriving them means the four differ in
/// hue and in nothing else.
///
/// COLOUR IS NEVER THE ONLY SIGNAL. Roughly one man in twelve cannot separate this green from this
/// red, and this page's whole job is to be read at a glance - so each state also has its own SHAPE,
/// and the value beside it says in words what happened. Colour is the third encoding, not the first.
/// </summary>
internal static class NetStatusLook
{
    public static Color Colour(CheckStatus s) => s switch
    {
        CheckStatus.Pass => Color.FromArgb(0x3B, 0xEA, 0x84),
        CheckStatus.Warn => LauncherTheme.Caution,
        CheckStatus.Fail => Color.FromArgb(0xEA, 0x3B, 0x3B),
        _ => LauncherTheme.TextDim,
    };

    /// <summary>Filled circle, triangle, cross, hollow circle.</summary>
    public static string Glyph(CheckStatus s) => s switch
    {
        CheckStatus.Pass => "●",
        CheckStatus.Warn => "▲",
        CheckStatus.Fail => "✕",
        _ => "○",
    };
}

internal sealed partial class OptionsPanel
{
    // ===========================================================================================
    // State, held on the panel so it outlives the page being rebuilt.
    // ===========================================================================================

    private readonly Dictionary<string, CheckResult> _netResults = new(StringComparer.Ordinal);
    private DateTimeOffset _netRanAt;
    private bool _netRan;
    private bool _netRunning;
    private CancellationTokenSource? _netCts;

    // Rebuilt with the page; nulled by Select() clearing _content, so every use is guarded.
    private readonly Dictionary<string, CheckRow> _netRowControls = new(StringComparer.Ordinal);
    private WrapNote? _netVerdict;
    private TextBox? _netCopyBox;
    private BigButton? _netRunBtn;
    private BigButton? _netCopyBtn;

    /// <summary>
    /// The NETWORK page's test section. Called from BuildNetwork; everything above it there is the
    /// read-only discovery note that page already carried.
    /// </summary>
    private void BuildNetworkTest()
    {
        _netRowControls.Clear();

        _netRunBtn = new BigButton(_netRunning ? "RUNNING…" : "RUN NETWORK TEST",
            LauncherTheme.PlayFace, LauncherTheme.PlayBorder)
        {
            Size = new Size(260, 46),
            Enabled = !_netRunning,
        };
        _netRunBtn.Click += (_, _) => _ = RunNetworkTestAsync();
        Row(_netRunBtn, 54, wide: false);

        Note("Checks the things that actually stop a join, in the order they usually cause one.");

        // THE VERDICT SITS ABOVE THE EVIDENCE. A reader who takes one thing from this page should
        // take the sentence, not a dot.
        _netVerdict = new WrapNote(new Font("Segoe UI", 10f, FontStyle.Bold))
        {
            Text = _netRan
                ? NetworkProbe.Verdict(OrderedResults())
                : "Not run yet. Nothing here is sent anywhere and nothing about you is measured — "
                  + "no public IP, no file paths, no user name.",
            ForeColor = _netRan ? LauncherTheme.Text : LauncherTheme.TextDim,
        };
        // Measured at the width it is about to be given, then stacked with that height - the same
        // two lines PreviewStrip and HudAnchorPreview already use. LayoutRows re-derives both from
        // IReflowsToWidth afterwards, but a row built with a height of zero stacks every row under
        // it on the same pixel until it does, which flashes on the way in.
        _netVerdict.Relayout(RowWidth);
        Row(_netVerdict, _netVerdict.Height + 8);

        foreach (var (key, name) in NetworkProbe.RowOrder)
        {
            var row = new CheckRow();
            row.Set(_netResults.TryGetValue(key, out var have)
                ? have
                : new CheckResult(key, name, CheckStatus.Pending, _netRan ? "checking…" : "not run", ""));
            row.Relayout(RowWidth);
            _netRowControls[key] = row;
            Row(row, row.Height + 8);
        }

        Note("Paste this line when you ask for help — it is enough to diagnose without follow-up questions:");

        // A SELECTABLE BOX, NOT A LABEL, and that is a failure mode rather than a style. Clipboard
        // access can be refused outright by whichever program is holding it, and on that machine a
        // COPY button is the only route to the line if the text cannot also be selected by hand.
        _netCopyBox = new TextBox
        {
            ReadOnly = true, Multiline = false, BorderStyle = BorderStyle.FixedSingle,
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            Font = new Font("Consolas", 9f), Height = 26,
            Text = _netRan ? NetworkProbe.CopyLine(OrderedResults(), _netRanAt) : "",
        };
        Row(_netCopyBox, 32);

        _netCopyBtn = new BigButton("COPY", LauncherTheme.Bg, LauncherTheme.Border)
        {
            Ghost = true, Size = new Size(150, 38), Enabled = _netRan,
        };
        _netCopyBtn.Click += (_, _) => CopyNetworkLine();
        Row(_netCopyBtn, 46, wide: false);
    }

    private IReadOnlyList<CheckResult> OrderedResults()
    {
        var list = new List<CheckResult>();
        foreach (var (key, name) in NetworkProbe.RowOrder)
        {
            list.Add(_netResults.TryGetValue(key, out var r)
                ? r
                : new CheckResult(key, name, CheckStatus.Pending, "checking…", ""));
        }
        return list;
    }

    private void CopyNetworkLine()
    {
        var line = _netCopyBox?.Text ?? "";
        if (line.Length == 0 || _netCopyBtn is null)
        {
            return;
        }
        try
        {
            Clipboard.SetText(line);
            _netCopyBtn.Text = "COPIED";
        }
        catch (Exception)
        {
            // Another program holds the clipboard. Say what to do instead of what went wrong -
            // the line is already on screen and selectable, which is the whole reason it is a box.
            _netCopyBtn.Text = "SELECT IT ABOVE";
        }
        _netCopyBtn.Invalidate();
    }

    /// <summary>
    /// Run the checks off the UI thread and repaint each row as it settles.
    ///
    /// NOTHING HERE BLOCKS THE WINDOW. Every probe carries its own timeout inside NetworkProbe, the
    /// whole run carries a second one here, and results arrive through a Progress created on this
    /// thread - so the callback marshals itself back and the render code never touches a control
    /// from a worker. A row can therefore never be left on "checking…" by a lookup that never
    /// returns, which is the specific way a diagnostic page becomes the thing that needs diagnosing.
    /// </summary>
    private async Task RunNetworkTestAsync()
    {
        if (_netRunning)
        {
            return;
        }
        _netRunning = true;
        _netRan = true;
        _netRanAt = DateTimeOffset.UtcNow;
        _netResults.Clear();

        _netCts?.Cancel();
        _netCts?.Dispose();
        _netCts = new CancellationTokenSource();
        // A ceiling over the whole run, above every individual probe's own. Reaching it means
        // something is wedged in a way no single timeout covered, and the rows that did answer are
        // still on screen and still true.
        _netCts.CancelAfter(TimeSpan.FromSeconds(45));
        var ct = _netCts.Token;

        if (_netRunBtn is { IsDisposed: false, Parent: not null })
        {
            _netRunBtn.Text = "RUNNING…";
            _netRunBtn.Enabled = false;
        }
        if (_netCopyBtn is { IsDisposed: false, Parent: not null })
        {
            _netCopyBtn.Enabled = false;
            _netCopyBtn.Text = "COPY";
        }
        RenderNetwork();

        var dir = OwnedPaths.InstallDir;
        var input = new NetworkProbe.ProbeInputs(
            PatchDir: Path.Combine(dir, NetworkProbe.PatchDirName),
            CachedManifestWire: ReadCachedManifest(dir),
            InstalledCommit: ReadInstalledCommit(dir),
            InstalledOrdinal: ReadInstalledOrdinal(dir),
            DebugMenuOn: GameOptions.ReadDebugMenu(),
            CheckUpdate: c => Apply.CheckAsync(dir, c));

        var progress = new Progress<CheckResult>(r =>
        {
            _netResults[r.Key] = r;
            RenderNetwork();
        });

        try
        {
            await NetworkProbe.RunAllAsync(input, progress, ct).ConfigureAwait(true);
        }
        catch (Exception)
        {
            // A run that threw is not a dialog. Whatever answered is on screen, whatever did not
            // is still Pending, and Verdict() reports an incomplete run as incomplete.
        }
        finally
        {
            _netRunning = false;
            if (_netRunBtn is { IsDisposed: false, Parent: not null })
            {
                _netRunBtn.Text = "RUN NETWORK TEST";
                _netRunBtn.Enabled = true;
            }
            RenderNetwork();
        }
    }

    /// <summary>
    /// Push the held results into the page's controls, if the page is still on screen.
    ///
    /// LIVENESS IS TESTED BY PARENT, NOT BY IsDisposed, and the difference is the whole reason this
    /// guard exists. A run outlives the page: Select() calls _content.Controls.Clear() on a tab
    /// click, which UNPARENTS every control without disposing any of them - so IsDisposed is still
    /// false on a row that is no longer anywhere, and a guard written against it would keep
    /// painting orphans and re-running the stacker over a page the user is not looking at. Parent
    /// goes null on exactly the event that matters.
    ///
    /// The results themselves live on the panel, so switching away and back rebuilds the page and
    /// re-renders the run that is still going.
    /// </summary>
    private void RenderNetwork()
    {
        if (_netVerdict is not { IsDisposed: false, Parent: not null })
        {
            return;
        }

        foreach (var (key, name) in NetworkProbe.RowOrder)
        {
            if (!_netRowControls.TryGetValue(key, out var row) || row.IsDisposed || row.Parent is null)
            {
                continue;
            }
            row.Set(_netResults.TryGetValue(key, out var r)
                ? r
                : new CheckResult(key, name, CheckStatus.Pending, _netRan ? "checking…" : "not run", ""));
        }

        var results = OrderedResults();

        _netVerdict.ForeColor = LauncherTheme.Text;
        _netVerdict.Text = NetworkProbe.Verdict(results);

        // The line is written as soon as anything has settled, not only at the end: somebody whose
        // run is stuck on one probe still has seven useful answers, and "verdict=INCOMPLETE" in the
        // line says exactly that rather than pretending the run finished.
        var anySettled = results.Any(r => r.Status != CheckStatus.Pending);
        if (_netCopyBox is { IsDisposed: false, Parent: not null })
        {
            _netCopyBox.Text = anySettled ? NetworkProbe.CopyLine(results, _netRanAt) : "";
        }
        if (_netCopyBtn is { IsDisposed: false, Parent: not null })
        {
            _netCopyBtn.Enabled = anySettled;
        }

        // Row heights move when a value grows from "checking…" to a sentence, so the stack has to be
        // re-run. LayoutRows guards its own re-entry.
        LayoutRows();
    }

    // ===========================================================================================
    // The three facts read off disk. Deliberately duplicated in shape from LauncherForm's own
    // readers rather than shared: those are private to the form, and this page must not be able to
    // change what the main panel prints.
    // ===========================================================================================

    /// <summary>
    /// Run the same probe the button runs, with no window, and print what it found.
    ///
    /// WHY THIS EXISTS. The network test is the screen people are told to open when a join fails,
    /// and until now the only way to see what it reports was to be sitting at the machine. That
    /// makes it the one diagnostic that cannot be collected remotely - and it also made the page
    /// unverifiable in any automated way, so "the four checks are real" was a claim nobody could
    /// re-run. Same ProbeInputs, same RunAllAsync, same Verdict as the UI path; only the rendering
    /// differs.
    /// </summary>
    internal static async Task<int> RunNetworkTestHeadlessAsync(string dir, System.IO.TextWriter w)
    {
        var input = new NetworkProbe.ProbeInputs(
            PatchDir: Path.Combine(dir, NetworkProbe.PatchDirName),
            CachedManifestWire: ReadCachedManifest(dir),
            InstalledCommit: ReadInstalledCommit(dir),
            InstalledOrdinal: ReadInstalledOrdinal(dir),
            DebugMenuOn: GameOptions.ReadDebugMenu(),
            CheckUpdate: c => Apply.CheckAsync(dir, c));

        var rows = new Dictionary<string, CheckResult>(StringComparer.Ordinal);
        var progress = new Progress<CheckResult>(r => rows[r.Key] = r);

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(45));
        try
        {
            await NetworkProbe.RunAllAsync(input, progress, cts.Token).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            w.WriteLine("run threw: " + ex.GetType().Name);
        }

        var list = rows.Values.ToList();
        foreach (var r in list)
        {
            w.WriteLine($"{r.Status,-8} {r.Name,-24} {r.Value}");
        }
        w.WriteLine();
        w.WriteLine("VERDICT: " + NetworkProbe.Verdict(list));
        // Non-zero when anything actually failed, so a caller can gate on it.
        return list.Any(r => r.Status == CheckStatus.Fail) ? 1 : 0;
    }

    private static string ReadCachedManifest(string dir)
    {
        try
        {
            var f = Path.Combine(dir, NetworkProbe.ManifestCacheFile);
            if (File.Exists(f) && new FileInfo(f).Length <= 64 * 1024)
            {
                return File.ReadAllText(f);
            }
        }
        catch { /* no cache is a first run, not an error */ }
        return "";
    }

    /// <summary>
    /// The engine commit, from the applied artifact's own name in gxinstalled.txt -
    /// "GeneralsX-win64-&lt;commit&gt;.zip". Same shape LauncherForm reads for the Build line, so the
    /// two screens cannot disagree about which build this is.
    /// </summary>
    private static string ReadInstalledCommit(string dir)
    {
        try
        {
            var f = Path.Combine(dir, "gxinstalled.txt");
            if (!File.Exists(f)) return "";
            foreach (var raw in File.ReadLines(f))
            {
                var l = raw.Trim();
                if (!l.StartsWith("item ", StringComparison.OrdinalIgnoreCase)) continue;
                var m = System.Text.RegularExpressions.Regex.Match(l, @"-win64-([0-9a-f]{7,40})(?:-|\.)");
                if (m.Success) return m.Groups[1].Value;
            }
        }
        catch { /* an unreadable state file leaves the row saying "unknown", which is honest */ }
        return "";
    }

    private static long ReadInstalledOrdinal(string dir)
    {
        try
        {
            var f = Path.Combine(dir, "gxinstalled.txt");
            if (!File.Exists(f)) return 0;
            foreach (var raw in File.ReadLines(f))
            {
                var l = raw.Trim();
                // The '#' banner is the first line; reading it blindly is how the Build label once
                // showed the banner text.
                if (l.Length == 0 || l.StartsWith('#') || l.StartsWith(';')) continue;
                if (l.StartsWith("item ", StringComparison.OrdinalIgnoreCase)) continue;
                var parts = l.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                var n = parts.FirstOrDefault(p => p.All(char.IsDigit));
                if (n is not null &&
                    long.TryParse(n, NumberStyles.None, CultureInfo.InvariantCulture, out var v))
                {
                    return v;
                }
            }
        }
        catch { /* 0 is the documented "nothing installed" ordinal, which is what we then send */ }
        return 0;
    }
}

/// <summary>
/// A label that owns its own height.
///
/// The options pane stacks rows by hand, and LayoutRows asks an <see cref="IReflowsToWidth"/> row
/// for its current Height instead of replaying the one it was built with - so a control whose text
/// wraps has to measure itself when its width changes, or it overlaps whatever is under it. That
/// fault paints like a rendering bug and reads nothing like the missing case in a stacker that it
/// actually is, which is why the interface exists at all.
/// </summary>
internal sealed class WrapNote : Label, IReflowsToWidth
{
    private int _laidFor = -1;

    public WrapNote(Font font)
    {
        AutoSize = false;
        UseMnemonic = false;
        Font = font;
        ForeColor = LauncherTheme.Text;
        BackColor = Color.Transparent;
        TextAlign = ContentAlignment.TopLeft;
    }

    protected override void OnTextChanged(EventArgs e)
    {
        base.OnTextChanged(e);
        _laidFor = -1;
        Relayout(Width);
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        // Comparing against the width last laid out for is what stops assigning Height from
        // recursing back through here forever.
        if (Width != _laidFor)
        {
            Relayout(Width);
        }
    }

    public void Relayout(int width)
    {
        if (width <= 0)
        {
            return;
        }
        _laidFor = width;
        var h = TextRenderer.MeasureText(Text, Font, new Size(width, int.MaxValue),
            TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height;
        Height = Math.Max(20, h + 4);
    }
}

/// <summary>
/// One check: shape, name, and what was measured.
///
/// THE VALUE COLUMN WRAPS RATHER THAN ELLIPSISING, and that decision is the difference between a
/// page that diagnoses and one that decorates. The useful half of a failed check is the reason -
/// "the host answered, but nothing is listening on UDP 8086" - and a row that clipped it to
/// "the host answ…" would leave the reader with a red dot and no idea what to do, which is what
/// every dashboard that only ever shows healthy rows ends up shipping.
/// </summary>
internal sealed class CheckRow : Panel, IReflowsToWidth
{
    private const int GlyphW = 22;
    private const int Gap = 10;
    private const int MinHeight = 26;

    private readonly Label _glyph = new()
    {
        AutoSize = false, UseMnemonic = false, TextAlign = ContentAlignment.TopCenter,
        Font = new Font("Segoe UI", 10f), BackColor = Color.Transparent,
    };

    private readonly Label _name = new()
    {
        AutoSize = false, UseMnemonic = false, TextAlign = ContentAlignment.TopLeft,
        Font = new Font("Segoe UI", 9.5f, FontStyle.Bold), ForeColor = LauncherTheme.Text,
        BackColor = Color.Transparent,
    };

    private readonly Label _value = new()
    {
        AutoSize = false, UseMnemonic = false, TextAlign = ContentAlignment.TopLeft,
        Font = new Font("Segoe UI", 9.5f), ForeColor = LauncherTheme.TextDim,
        BackColor = Color.Transparent,
    };

    private int _laidFor = -1;

    public CheckRow()
    {
        BackColor = LauncherTheme.Bg;
        Height = MinHeight;
        Controls.Add(_glyph);
        Controls.Add(_name);
        Controls.Add(_value);
    }

    public void Set(CheckResult r)
    {
        _glyph.Text = NetStatusLook.Glyph(r.Status);
        _glyph.ForeColor = NetStatusLook.Colour(r.Status);
        _name.Text = r.Name;
        _value.Text = r.Value;
        // A failure's reason is the thing to read on the row; a pass is context. Dimming the passes
        // is what makes the one red row findable without making the page shout.
        _value.ForeColor = r.Status == CheckStatus.Fail
            ? LauncherTheme.Text
            : LauncherTheme.TextDim;
        _laidFor = -1;
        Relayout(Width);
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        if (Width != _laidFor)
        {
            Relayout(Width);
        }
    }

    public void Relayout(int width)
    {
        if (width <= 0)
        {
            return;
        }
        _laidFor = width;

        // The name column takes 30% and is clamped, so a wide window does not leave a corridor of
        // empty space between the label and its value while a narrow one still fits "Relay round
        // trip (UDP)" on one line.
        var nameW = Math.Clamp(width * 30 / 100, 120, 210);
        var valueX = GlyphW + Gap + nameW + Gap;
        var valueW = Math.Max(80, width - valueX);

        var valueH = TextRenderer.MeasureText(_value.Text, _value.Font, new Size(valueW, int.MaxValue),
            TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height;
        var h = Math.Max(MinHeight, valueH + 6);

        _glyph.Bounds = new Rectangle(0, 1, GlyphW, 20);
        _name.Bounds = new Rectangle(GlyphW + Gap, 2, nameW, Math.Min(h - 2, 20));
        _value.Bounds = new Rectangle(valueX, 2, valueW, h - 4);
        Height = h;
    }
}
