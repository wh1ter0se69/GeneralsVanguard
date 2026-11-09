/* The changelog, as a view inside the launcher's own frame.
 *
 * GeneralsX @feature Claude 13/08/2026
 *
 * SAME IDIOM AS OPTIONS, ON PURPOSE. OptionsPanel is a Panel that swaps into the launcher's frame
 * and swaps back out - one window, one taskbar entry, one close button - with a sidebar of choices
 * and one content pane. This is that, with releases where Options has categories. A third
 * navigation idiom (a dialog, a browser window) would be a third thing to explain, and opening the
 * system browser would take the user out of the launcher entirely to read two paragraphs.
 *
 * EVERY CONTROL THAT SHOWS FETCHED TEXT SETS UseMnemonic = false. That is not a style rule here, it
 * is the injection defence: WinForms Labels treat '&' as an accelerator marker, so a release note
 * reading "R&D work" would render as "RD work" with a D underlined. The document is fetched from
 * the network and is not signed - see Changelog.cs for what else is stripped before it gets here.
 *
 * GeneralsX @bugfix Claude 17/08/2026 THE NOTES NOW REFLOW WITH THE WINDOW. They did not, and the
 * symptom was words cut off down the right-hand edge with no way to read them.
 *
 * The launcher became resizable on 16/08/2026 (LauncherForm.PositionChrome, MinimumSize 900x480,
 * no maximum) and this panel was built for a window that could not change size: every label got
 * absolute Bounds computed ONCE, from whatever width the content pane happened to have at the
 * moment a release was selected. Widening the window then left every note at its old width, and -
 * worse, because it is the case people actually hit - a note measured at one width and re-shown at
 * a narrower one wrapped onto lines the label was no longer tall enough to display, so the tail of
 * it was simply not painted. Nothing about that reads as "the window is too small"; it reads as a
 * truncated changelog.
 *
 * THE FIX EXTENDS THE CONTRACT THAT ALREADY EXISTS RATHER THAN INVENTING A SECOND ONE.
 * IReflowsToWidth (HudAnchorPreview.cs) is how this project already says "my height is a function
 * of my width, ask me again when the width changes"; GamePreview and HudAnchorPreview both
 * implement it. NotesPanel below is the third, and the only new thing is that it derives its height
 * from MEASURED TEXT rather than from an aspect ratio.
 *
 * THREE THINGS RESIZE, not one, and the two smaller ones were wrong from the start rather than
 * broken by the resize work: the sidebar rows were built 32px wider than the list that holds them
 * (Width = _side.Width, inside a _side with 16px of padding on each side), so the right-hand end of
 * every title was clipped at every window size; and the empty-state text was a fixed 560x140 block
 * that a 900px window cannot show in full.
 */

namespace GeneralsX.Loader;

/// <summary>
/// A stack of wrapped paragraphs whose height is whatever the text needs at the current width.
///
/// WHY MEASURED AND NOT AutoSize. A WinForms Label with AutoSize = true grows sideways, not
/// downwards - it would make one very long line and push a horizontal scrollbar across the pane,
/// which is the exact failure OptionsPanel documents at length. AutoSize = false wraps, but then
/// the height is the caller's problem, and a fixed height is what clipped these notes in the first
/// place. Measuring with the same width the label will actually get is the only arrangement where
/// what is displayed and what was reserved for it cannot disagree.
/// </summary>
internal sealed class NotesPanel : Panel, IReflowsToWidth
{
    /// <summary>How loud a paragraph is.</summary>
    internal enum Tone
    {
        /// <summary>Body copy.</summary>
        Body,

        /// <summary>Secondary text: an absence, a caveat, small print.</summary>
        Dim,

        /// <summary>Something the reader has to act on. Amber, and used sparingly or not at all.</summary>
        Caution,
    }

    /// <summary>One paragraph and how loudly to say it.</summary>
    internal readonly record struct Line(string Text, Tone Tone);

    private readonly List<Label> _labels = new();
    private readonly float _fontSize;

    /// <summary>
    /// The width the current layout was computed for. Compared against on every SizeChanged so that
    /// assigning our own Size from inside Relayout cannot recurse - the same guard GamePreview and
    /// HudAnchorPreview use, for the same reason.
    /// </summary>
    private int _laidOutFor = -1;

    private const int Gap = 8;

    /// <summary>
    /// Slack added to the measured height of every paragraph.
    ///
    /// TextRenderer.MeasureText and the Label's own painting agree to within a pixel or two, not
    /// exactly - they round line boxes independently - and the two failures are not symmetric: a
    /// few spare pixels are invisible, while being one pixel short cuts the descenders off the last
    /// line and reads as a font-rendering fault rather than a layout one.
    /// </summary>
    private const int MeasureSlack = 6;

    /// <summary>
    /// <paramref name="fontSize"/> is a parameter because this is used at two sizes: 10pt for the
    /// changelog's release notes, and 8.5pt for the one-line status under an options checkbox. The
    /// second caller is the reason this class is not private to the changelog - a status line that
    /// grows to two or three lines at a narrow window is the same problem as a release note that
    /// does, and it was clipped in exactly the state where it matters most.
    /// </summary>
    public NotesPanel(float fontSize = 10f)
    {
        _fontSize = fontSize;
        BackColor = LauncherTheme.Bg;
    }

    public void SetLines(IEnumerable<Line> lines)
    {
        foreach (var lb in _labels)
        {
            Controls.Remove(lb);
            lb.Dispose();
        }
        _labels.Clear();

        foreach (var line in lines)
        {
            var lb = new Label
            {
                Text = line.Text,
                AutoSize = false, UseMnemonic = false,
                ForeColor = line.Tone switch
                {
                    Tone.Caution => LauncherTheme.Caution,
                    Tone.Dim => LauncherTheme.TextDim,
                    _ => LauncherTheme.Text,
                },
                Font = new Font("Segoe UI", _fontSize),
                TextAlign = ContentAlignment.TopLeft,
            };
            _labels.Add(lb);
            Controls.Add(lb);
        }

        _laidOutFor = -1;           // force a re-measure: the text changed, so the height did too
        Relayout(Width);
    }

    public void Relayout(int width)
    {
        var avail = Math.Max(120, width);
        var y = 0;
        foreach (var lb in _labels)
        {
            var h = TextRenderer.MeasureText(lb.Text, lb.Font, new Size(avail, 0),
                TextFormatFlags.WordBreak).Height + MeasureSlack;
            lb.Bounds = new Rectangle(0, y, avail, h);
            y += h + Gap;
        }
        _laidOutFor = avail;
        Size = new Size(avail, Math.Max(1, y));
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        if (Width > 0 && Width != _laidOutFor)
        {
            Relayout(Width);
        }
    }
}

internal sealed class ChangelogPanel : Panel
{
    private readonly Panel _side = new() { Dock = DockStyle.Left, BackColor = LauncherTheme.Panel };
    private readonly Panel _list = new() { Dock = DockStyle.Fill, BackColor = LauncherTheme.Panel, AutoScroll = true };
    private readonly Panel _pane = new()
    {
        Dock = DockStyle.Fill, BackColor = LauncherTheme.Bg, Padding = new Padding(28, 24, 28, 24),
    };

    private readonly Label _heading = new()
    {
        Dock = DockStyle.Top, Height = 40, AutoSize = false, UseMnemonic = false,
        ForeColor = Color.White, Font = new Font("Segoe UI", 15f, FontStyle.Bold),
        TextAlign = ContentAlignment.TopLeft,
    };

    private readonly Label _stamp = new()
    {
        Dock = DockStyle.Top, Height = 26, AutoSize = false, UseMnemonic = false,
        ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9.5f),
        TextAlign = ContentAlignment.MiddleLeft,
    };

    private readonly Panel _content = new()
    {
        Dock = DockStyle.Fill, BackColor = LauncherTheme.Bg, AutoScroll = true,
    };

    private readonly NotesPanel _notes = new();
    private readonly Panel _selBar = new() { BackColor = LauncherTheme.Accent };
    private readonly List<Label> _rows = new();
    private readonly Changelog _log;
    private int _sel = -1;

    /// <summary>
    /// The sidebar's width, from the window's.
    ///
    /// A FIXED 240 was fine while the window was fixed. It is not fine across a range that starts at
    /// 900px and has no upper bound: at the floor, 240 of 900 leaves the detail pane 604px after its
    /// padding, which is workable; maximised on a wide screen the same 240 leaves a list of
    /// two-line titles squeezed beside an ocean of empty detail pane. The clamp keeps both halves
    /// usable at both ends - never below the width a date and a title need, never past the point
    /// where the list is just a wide margin.
    /// </summary>
    private int SideWidth => Math.Clamp(ClientSize.Width * 28 / 100, 240, 380);

    public ChangelogPanel(Changelog log, Action onDone, Size size)
    {
        _log = log;
        Dock = DockStyle.Fill;
        BackColor = LauncherTheme.Bg;
        Size = size;
        _side.Width = Math.Clamp(size.Width * 28 / 100, 240, 380);

        // GeneralsX @bugfix Claude 13/08/2026 GO BACK IS DOCKED, NOT PLACED. It used to be given
        // absolute Bounds derived from the `size` argument and then BringToFront()ed over a
        // Dock=Fill list. Two things had to hold for that to be visible - `size` had to equal the
        // sidebar's real laid-out height, and the z-order shuffle had to not disturb the dock
        // order - and on the shipped 06:42 launcher it was not visible at all: the changelog view
        // had no way back except closing the window.
        //
        // Docking removes both assumptions. WinForms lays docked children out in REVERSE z-order,
        // so the Bottom strip must be added AFTER the Fill list to claim its space first; that is
        // also exactly why the old BringToFront() was dangerous here, and it is gone.
        var back = new BigButton("GO BACK", LauncherTheme.PlayFace, LauncherTheme.PlayBorder)
        {
            Dock = DockStyle.Bottom,
            Height = 52,
        };
        back.Click += (_, _) => onDone();

        // Inset the whole sidebar instead of padding the list around a floating button.
        _side.Padding = new Padding(16, 0, 16, 16);
        _list.Padding = new Padding(0, 12, 0, 12);

        _content.Controls.Add(_notes);
        _pane.Controls.Add(_content);
        _pane.Controls.Add(_stamp);
        _pane.Controls.Add(_heading);
        Controls.Add(_pane);
        _side.Controls.Add(_list);
        _side.Controls.Add(back);
        Controls.Add(_side);

        // The selection stripe goes INSIDE the scrolling list, not beside it in the sidebar.
        // As a child of _side it was positioned in the list's coordinates but clipped by the
        // sidebar's, and the sidebar is 52px taller because it also holds the Go Back button - so
        // once there were enough releases to scroll, the selected row's stripe kept being painted
        // (and BringToFront'd) straight over that button. Parenting it here makes the list's own
        // rectangle the clip, which is the boundary it was always supposed to respect, and lets
        // WinForms move it with the rows instead of us re-deriving its position on every scroll.
        _list.Controls.Add(_selBar);

        // Same enforced vertical-only scrolling as OptionsPanel: a stray pixel of horizontal scroll
        // clips the first character of every line rather than looking like a layout bug.
        ScrollUtil.VerticalOnly(_content, _list);

        // The three things that have to follow the window. Wired before anything is built so the
        // first real layout pass - which arrives after the form adds this panel, not during this
        // constructor - lays out what is already here.
        _content.SizeChanged += (_, _) => LayoutDetail();
        _list.SizeChanged += (_, _) => LayoutSidebar();

        if (!_log.Any)
        {
            ShowEmpty();
            return;
        }

        for (var i = 0; i < _log.Releases.Count; i++)
        {
            var idx = i;
            var row = new Label
            {
                // Date over title would need two controls per row; the date first in one line keeps
                // the list scannable and the whole row is one hit target.
                // Date and clock time here, seconds only in the detail pane: the sidebar is narrow
                // and the title has to stay readable beside it.
                Text = RowText(_log.Releases[i]),
                AutoSize = false, UseMnemonic = false,
                TextAlign = ContentAlignment.MiddleLeft, Cursor = Cursors.Hand,
                ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9f),
            };
            row.Click += (_, _) => Select(idx);
            _rows.Add(row);
            _list.Controls.Add(row);
        }

        LayoutSidebar();
        Select(0);
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        // Guard the constructor-time call: this fires while the base sets Size, before _side is a
        // child of anything.
        if (Controls.Contains(_side) && _side.Width != SideWidth)
        {
            _side.Width = SideWidth;
        }
    }

    private static string RowText(ChangelogEntry r) =>
        $"   {Stamp(r, "yyyy-MM-dd HH:mm")}\n   {Heading(r)}";

    /// <summary>
    /// Publish time to the second, DERIVED not transported.
    ///
    /// GeneralsX @feature Claude 13/08/2026 The document's date field is YYYY-MM-DD and cannot carry
    /// more: Changelog.cs validates it at exactly ten characters and splits the release header on
    /// spaces, so widening the field would make every ALREADY-INSTALLED launcher reject the whole
    /// document and show "no changelog available". The ordinal is the publish epoch, so the clock
    /// time comes from there and old and new launchers keep reading identical bytes. Falls back to
    /// the plain date if an ordinal is not a plausible epoch, because this text is hostile input.
    /// </summary>
    private static string Stamp(ChangelogEntry r, string format) =>
        r.Ordinal is > 1_000_000_000UL and < 4_000_000_000UL
            ? DateTimeOffset.FromUnixTimeSeconds((long)r.Ordinal).ToLocalTime().ToString(format)
            : r.Date;

    /// <summary>
    /// A release with no title of its own is headed by its date rather than by a placeholder.
    ///
    /// Changelog.cs already substitutes "(no title)" so that nothing downstream sees an empty
    /// string, and a heading in parentheses reads as a fault in the launcher rather than as an
    /// omission in one release note. Testing for empty AND for that substitute keeps this correct
    /// whichever of the two arrives, without either file having to know what the other does.
    /// </summary>
    private static string Heading(ChangelogEntry r) =>
        r.Title.Trim().Length == 0 || r.Title == "(no title)" ? "Release " + r.Date : r.Title;

    private void ShowEmpty()
    {
        _heading.Text = "CHANGELOG";
        _stamp.Text = "";
        _notes.SetLines(new[]
        {
            new NotesPanel.Line("No changelog available.", NotesPanel.Tone.Dim),
            new NotesPanel.Line(
                "The release notes are fetched from the update server. This launcher could not reach " +
                "it, or this release predates the changelog. Nothing is wrong with your install, and " +
                "Play works normally.", NotesPanel.Tone.Dim),
        });
        LayoutDetail();
    }

    /// <summary>
    /// Re-width and re-stack the sidebar rows.
    ///
    /// THE WIDTH COMES FROM THE LIST, NOT FROM THE SIDEBAR. That one substitution is the whole bug:
    /// rows were built at _side.Width while living inside _list, which _side's own 16px-a-side
    /// padding makes 32px narrower - so the last characters of every title were clipped at every
    /// window size, and the sidebar looked like it had been designed for shorter titles.
    /// </summary>
    private void LayoutSidebar()
    {
        var w = _list.ClientSize.Width;
        if (w <= 0)
        {
            w = _list.Width;
        }
        w = Math.Max(120, w - SystemInformation.VerticalScrollBarWidth);

        var y = 4;
        foreach (var row in _rows)
        {
            row.Bounds = new Rectangle(0, y, w, 52);
            y += 52;
        }
        PositionSelBar();
    }

    private void PositionSelBar()
    {
        if (_sel < 0 || _sel >= _rows.Count)
        {
            _selBar.Visible = false;
            return;
        }
        _selBar.Visible = true;
        // The stripe and the rows are siblings in the same scrolling client area, so the row's own
        // Top is already the right coordinate - no _list.Top, and no AutoScrollPosition. Adding
        // either one double-counts the scroll, and the sidebar's rectangle no longer enters into it
        // at all now that the list is what clips this.
        _selBar.Bounds = new Rectangle(0, _rows[_sel].Top, 3, _rows[_sel].Height);
        _selBar.BringToFront();
    }

    /// <summary>
    /// Re-measure the detail pane for its current width.
    ///
    /// The heading is measured too, not just the notes: a release title is one line of fetched text
    /// with no length limit short of Changelog.Clip, and at 900px a long one wrapped onto a second
    /// line that a fixed 40px-tall docked label had no room for. Clamped to three lines, because
    /// past that a title has stopped being a title and the notes below it are what matters.
    /// </summary>
    /// <summary>
    /// Guards LayoutDetail against re-entering itself. See the note in the method.
    /// </summary>
    private bool _layingDetail;

    private void LayoutDetail()
    {
        // GeneralsX @bugfix Claude 19/08/2026 W4. This is wired to _content.SizeChanged and then
        // resizes controls inside _content - which can move the vertical scrollbar in or out, which
        // changes _content.ClientSize, which raises SizeChanged again. OptionsPanel.LayoutRows has
        // carried exactly this guard since it started re-stacking rows; this one never got it, and
        // the two are the same shape of method with the same feedback path.
        //
        // Re-entrancy here does not crash, which is why it survived: it re-measures with a width
        // that is one scrollbar different from the one it started with, so the visible symptom is a
        // wrap point that moves by a word between two paints. Cheap to prevent, unpleasant to
        // diagnose from a screenshot.
        if (_layingDetail)
        {
            return;
        }
        _layingDetail = true;
        try
        {

        var w = _content.ClientSize.Width;
        if (w <= 0)
        {
            w = _content.Width;
        }
        // The vertical scrollbar's width comes off whether or not it is currently showing - the same
        // reasoning as OptionsPanel.RowWidth. Sizing to the full client width fits exactly until the
        // page grows tall enough to scroll, at which point every line is suddenly one scrollbar too
        // wide and AutoScroll adds a horizontal bar under content that fitted a moment earlier.
        if (!_content.VerticalScroll.Visible)
        {
            w -= SystemInformation.VerticalScrollBarWidth;
        }
        w = Math.Max(160, w);

        var headW = Math.Max(160, _pane.ClientSize.Width - _pane.Padding.Horizontal);
        var headH = TextRenderer.MeasureText(_heading.Text, _heading.Font, new Size(headW, 0),
            TextFormatFlags.WordBreak).Height + 8;
        _heading.Height = Math.Clamp(headH, 40, 3 * 34);

        _notes.Location = new Point(0, 6);
        _notes.Relayout(w);

        }
        finally
        {
            _layingDetail = false;
        }
    }

    private void Select(int i)
    {
        _sel = i;
        for (var k = 0; k < _rows.Count; k++)
        {
            _rows[k].BackColor = k == i ? LauncherTheme.SidebarSel : LauncherTheme.Panel;
            _rows[k].ForeColor = k == i ? Color.White : LauncherTheme.TextDim;
        }
        PositionSelBar();

        var r = _log.Releases[i];
        _heading.Text = Heading(r);
        _stamp.Text = Stamp(r, "yyyy-MM-dd HH:mm:ss") + $"      build {r.Commit}";

        // A release with no notes is a normal thing - a packaging-only publish has nothing to say -
        // so it gets a sentence rather than an empty pane, and the sentence is dim because it is
        // the absence of content rather than content.
        _notes.SetLines(r.Notes.Count == 0
            ? new[] { new NotesPanel.Line("No detail was recorded for this release.", NotesPanel.Tone.Dim) }
            : r.Notes.Select(n => new NotesPanel.Line("•  " + n, NotesPanel.Tone.Body)));

        LayoutDetail();
    }
}
