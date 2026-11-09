/* Options: a sidebar of categories and one pane of settings.
 *
 * GeneralsX @feature Claude 09/08/2026
 *
 * EVERY SETTING HERE IS ONE THIS LAUNCHER CAN ACTUALLY APPLY. That rule is the whole design. The
 * previous settings screen asked for a server address, a port and a "room" - two of which are
 * protocol nouns that mean nothing to a player, and all three of which the launcher now discovers
 * for itself. A settings screen that collects answers nobody has is worse than no settings screen,
 * because it blocks the install behind them.
 *
 * So what is here is what a person genuinely chooses: where it installed, whether it makes
 * shortcuts, windowed or fullscreen, a frame cap, and the diagnostics that make a bug report
 * useful. Network is deliberately READ-ONLY - it shows what was discovered, it does not ask.
 *
 * Settings live in gxlauncher.ini beside the launcher, NOT in the registry: the whole install is a
 * folder the user can delete, and a registry key would outlive it.
 */

using System.Globalization;             // InvariantCulture for the SagePatch numbers
using System.Runtime.InteropServices;   // EnumDisplaySettings, for the resolution picker

namespace GeneralsX.Loader;

/// <summary>
/// Options is a VIEW, not a window. It swaps into the launcher's own frame and swaps back out.
///
/// It used to be a second MetroForm opened with ShowDialog, which meant a second title bar, a
/// second close button, and a second thing on the taskbar for what is one program with one job.
/// A settings screen that spawns a window is a settings screen you have to manage.
/// </summary>
internal sealed partial class OptionsPanel : Panel
{
    private readonly Panel _side = new() { Dock = DockStyle.Left, Width = 190, BackColor = LauncherTheme.Panel };
    private readonly Panel _pane = new() { Dock = DockStyle.Fill, BackColor = LauncherTheme.Bg, Padding = new Padding(28, 24, 28, 24) };
    private readonly Label _heading = new()
    {
        Dock = DockStyle.Top, Height = 44, AutoSize = false, UseMnemonic = false,
        ForeColor = Color.White, Font = new Font("Segoe UI", 15f, FontStyle.Bold),
        TextAlign = ContentAlignment.MiddleLeft,
    };
    private readonly Panel _content = new() { Dock = DockStyle.Fill, BackColor = LauncherTheme.Bg, AutoScroll = true };

    private readonly Label _close = new()
    {
        Text = "✕", AutoSize = false, Size = new Size(40, 28), UseMnemonic = false,
        TextAlign = ContentAlignment.MiddleCenter, ForeColor = LauncherTheme.TextDim,
        Font = new Font("Segoe UI", 11f), Cursor = Cursors.Hand,
        Anchor = AnchorStyles.Top | AnchorStyles.Right,
    };

    // HUD sits next to GRAPHICS because that is what a person looking for "make the text bigger"
    // will try first, and it is a different kind of setting from the diagnostics further down: these
    // change what the game looks like while you play it, not what it records about itself.
    private static readonly string[] Categories = { "GENERAL", "GRAPHICS", "HUD", "KEYBINDS", "NETWORK", "DIAGNOSTICS", "ABOUT" };

    /// <summary>How many pages exist, so SnapAllViews photographs all of them.</summary>
    public static int CategoryCount => Categories.Length;
    private readonly List<Label> _tabs = new();
    private readonly Panel _selBar = new() { BackColor = LauncherTheme.Accent };
    private readonly LauncherSettings _s = LauncherSettings.Load();
    private int _sel;

    /// <summary>
    /// The one picture on the GENERAL page, and the state it is drawn from. All four are rebuilt
    /// with the page, because Select() clears the content pane and starts again.
    /// </summary>
    private GamePreview? _gamePreview;
    private PreviewArt _art = PreviewArt.Empty;
    private string _patchDir = "";
    private List<OptionalEntry> _optional = new();
    private readonly List<(OptionalEntry E, CheckBox Box, NotesPanel Status)> _optionRows = new();

    /// <summary>
    /// True when the extra content has been rebuilt since the embedded screenshot was captured, so
    /// the picture is of bytes this install no longer has.
    ///
    /// Computed once per page build rather than per repaint: gxinstalled.txt does not change while
    /// somebody is clicking checkboxes, and re-reading a file on every toggle to answer the same
    /// question is work nobody asked for.
    /// </summary>
    private bool _artStale;

    private readonly Action _onDone;

    public OptionsPanel(Action onDone, Size size)
    {
        _onDone = onDone;
        Dock = DockStyle.Fill;
        BackColor = LauncherTheme.Bg;
        Size = size;

        var save = new BigButton("SAVE & GO BACK", LauncherTheme.PlayFace, LauncherTheme.PlayBorder)
        {
            Bounds = new Rectangle(16, size.Height - 70, _side.Width - 32, 52),
            Anchor = AnchorStyles.Bottom | AnchorStyles.Left,
        };
        save.Click += (_, _) =>
        {
            _s.Save();

            // GeneralsX @bugfix Claude 13/08/2026 REFUSE TO WRITE Options.ini WHILE THE GAME IS
            // RUNNING, because writing it then is indistinguishable from not writing it at all.
            //
            // The engine loads Options.ini into a std::map at startup and dumps that entire map
            // back over the file when it exits (UserPreferences::load / ::write) - it does not
            // merge, and it does not re-read. So anything saved here mid-session is silently
            // reverted the moment the player quits: keys we appended disappear, keys we changed
            // return to whatever the game read half an hour ago. The user is left certain they
            // saved a setting that did not take.
            //
            // This gate covers every Options.ini writer below, not only the new HUD ones - the
            // resolution and the debug menu were always exposed to the same reversal. Nothing is
            // cleared and the screen does not close, so the pending choices survive and one more
            // SAVE after quitting the game applies them.
            //
            // SagePatch.ini is NOT gated: it is our file, the engine only ever reads it at startup
            // and writes it when it is absent, so there is no map to overwrite us with.
            if ((_resolution is not null || _debugMenu is not null || PendingHudKeys().Count > 0) &&
                BlockedByRunningGame())
            {
                return;
            }

            // The game's own Options.ini is only touched when the resolution was actually changed.
            // Opening this screen and pressing SAVE must not rewrite a file that belongs to the
            // game and is shared with retail Zero Hour and the Generals Online client.
            //
            // CLEARED ONCE WRITTEN, and that is what makes the sentence above true a second time.
            // This panel is built once and reused for the life of the process - LauncherForm does
            // `_optionsView ??= new OptionsPanel(...)` and only toggles Visible after that - so a
            // pending choice left set here outlives its own save, and every later SAVE writes it
            // again. With CloseOnPlay off the launcher outlives a whole game session, and the game
            // rewrites Options.ini when it exits: the second save would then silently undo a
            // resolution the user had just changed in-game. That is precisely the stale-copy
            // failure the picker refuses to mirror into gxlauncher.ini for, and an in-memory copy
            // is not exempt from it.
            //
            // Only on success, so a write that failed is still pending and is retried next time.
            if (_resolution is { } r && GameOptions.WriteResolution(r))
            {
                _resolution = null;
            }
            // Same rule for the debug menu: only when it was actually changed, only on success, and
            // cleared once written. This is the line that makes the checkbox do anything at all -
            // the engine reads DebugMenu from the game's Options.ini, not DebugMode from ours.
            if (_debugMenu is { } d && GameOptions.WriteDebugMenu(d))
            {
                _debugMenu = null;
            }
            // Camera settings go to SagePatch.ini, our own per-device file beside Options.ini, and
            // only when the user actually touched a camera control - the same write-on-change
            // discipline as the resolution above. Both pending values are written together so that
            // changing one does not reset the other to a default.
            if (_minHeight is not null || _movePct is not null)
            {
                var cur = GameSagePatch.ReadCamera();
                if (GameSagePatch.WriteCamera(_minHeight ?? cur.MinHeight, _movePct ?? cur.MovePct))
                {
                    _minHeight = null;
                    _movePct = null;
                }
            }
            // The HUD sizes, the income column and the hide-the-HUD key. Same write-on-change,
            // clear-on-success discipline as everything above it; see SaveHud in HudPage.cs.
            SaveHud();
            // GeneralsX @feature wh1ter0se 20/08/2026 The build hotkeys. Written only when the
            // Keybinds page actually changed something: this file is read by the engine on init
            // and on reset, and rewriting it on every SAVE would touch it for people who never
            // opened the page.
            if (_keysDirty && _keys is not null)
            {
                _keysDirty = !_keys.Save();
            }
            _onDone();
        };

        _pane.Controls.Add(_content);
        _pane.Controls.Add(_heading);
        Controls.Add(_pane);
        Controls.Add(_side);

        // VERTICAL SCROLLING ONLY, ENFORCED RATHER THAN RELIED ON. RowWidth sizes every row to fit
        // the viewport, but that is arithmetic about controls whose painting we do not own, and the
        // failure mode is bad out of proportion to the cause: one stray pixel gives a horizontal
        // scrollbar across the bottom of the screen and scrolls the pane sideways, which silently
        // clips the first character of every label rather than looking like a layout bug.
        //
        // The ordering this depends on is explained once, in ScrollUtil.VerticalOnly.
        ScrollUtil.VerticalOnly(_content);

        // Rows are placed and sized by hand, so their widths have to be re-applied by hand whenever
        // the panel's width changes - and it does change once, invisibly. This control is Dock=Fill
        // inside a Dock=Fill chain, so the width it has during this constructor is derived from the
        // caller's ClientSize; the real one arrives only after the form lays it out, smaller by
        // MetroForm's own 20px side padding. Sizing a row from the constructor-time width alone
        // leaves it wider than the panel it ends up in.
        _content.SizeChanged += (_, _) => LayoutRows();

        for (var i = 0; i < Categories.Length; i++)
        {
            var idx = i;
            var tab = new Label
            {
                Text = "   " + Categories[i], AutoSize = false, UseMnemonic = false,
                Bounds = new Rectangle(0, 18 + (i * 46), _side.Width, 46),
                TextAlign = ContentAlignment.MiddleLeft, Cursor = Cursors.Hand,
                ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold),
            };
            tab.Click += (_, _) => Select(idx);
            _tabs.Add(tab);
            _side.Controls.Add(tab);
        }
        _side.Controls.Add(save);
        _side.Controls.Add(_selBar);

        // No close glyph here - the launcher frame already has one, and a second ✕ inside the same
        // window is the "why two?" problem in miniature. Leaving is SAVE & GO BACK.
        Select(0);
    }

    /// <summary>
    /// Show one options page by index, for --options=&lt;n&gt; and --snap.
    ///
    /// GeneralsX @feature Claude 19/08/2026 Select() is private and driven by the sidebar, so five
    /// of the six pages had no way to be reached without synthetic mouse clicks on somebody's
    /// desktop. The restructure has to prove it changed none of them, which means photographing
    /// all six. Out of range is clamped rather than thrown: this is a diagnostic entry point and a
    /// crash here would be a worse answer than the ABOUT page.
    /// </summary>
    /// <summary>
    /// Scroll the content pane, for --snap. A page taller than the window is otherwise
    /// photographed only down to the fold, which is exactly where the controls tend to live once a
    /// page carries a preview - and an unphotographed control is an unreviewed one.
    /// </summary>
    public void ScrollContent(bool toBottom)
    {
        if (!_content.VerticalScroll.Visible)
        {
            return;
        }
        var v = toBottom ? _content.VerticalScroll.Maximum : _content.VerticalScroll.Minimum;
        _content.VerticalScroll.Value = Math.Max(_content.VerticalScroll.Minimum,
            Math.Min(_content.VerticalScroll.Maximum, v));
        _content.PerformLayout();
    }

    public void SelectPage(int i)
    {
        if (i < 0) i = 0;
        if (i >= Categories.Length) i = Categories.Length - 1;
        Select(i);
    }

    private void Select(int i)
    {
        _sel = i;
        // GeneralsX @bugfix Claude 17/08/2026 DISPOSE THE PICTURE BEFORE THE PAGE IS CLEARED.
        // Controls.Clear() REMOVES children without disposing them, which was harmless while every
        // row was a Label - and is not harmless now that one row owns a decoded base screenshot, a
        // decoded layer per option and a composed bitmap. At 1600x840 in 32bpp that is about 5 MB
        // per set, leaked on every click of a sidebar category, and a settings screen that grows
        // its own working set while you click around it is a bug people report as "the launcher
        // gets slow".
        _gamePreview?.Dispose();
        _gamePreview = null;
        _optionRows.Clear();
        for (var k = 0; k < _tabs.Count; k++)
        {
            _tabs[k].BackColor = k == i ? LauncherTheme.SidebarSel : LauncherTheme.Panel;
            _tabs[k].ForeColor = k == i ? Color.White : LauncherTheme.TextDim;
        }
        // The selection marker is a 3px accent stripe, not a coloured row. Same reason the rest of
        // the palette went neutral: blue marks the one thing that is selected, it is not the
        // surface everything sits on.
        _selBar.Bounds = new Rectangle(0, _tabs[i].Top, 3, _tabs[i].Height);
        _selBar.BringToFront();
        _heading.Text = Categories[i];
        _content.Controls.Clear();
        _wide.Clear();
        _rows.Clear();

        switch (i)
        {
            case 0: BuildGeneral(); break;
            case 1: BuildGraphics(); break;
            case 2: BuildHud(); break;
            case 3: BuildKeybinds(); break;
            case 4: BuildNetwork(); break;
            case 5: BuildDiagnostics(); break;
            default: BuildAboutReal(); break;
        }

        // The page was built at whatever width the panel has right now; SizeChanged only covers
        // changes from here on.
        LayoutRows();
    }

    // Simple vertical stacker. A TableLayoutPanel would reflow these on every theme change and the
    // rows are not tabular anyway.

    /// <summary>The rows that span the panel. Rebuilt with the page, like the rows themselves.</summary>
    private readonly List<Control> _wide = new();
    // GeneralsX @refactor Claude 19/08/2026 W3. GapBelow is the space under a row, and it is a
    // PARAMETER now rather than a cursor nudge. Every free-standing `_y += n` in this file, HudPage
    // and NetworkPage - 34 of them, 276px in total - was dead: LayoutRows re-derives every position
    // from this list starting at y = 0, and Select() always ends by calling it. The source
    // described a spaced page and the screen has always shown a flush stack.
    //
    // Measured before deleting them, on a 1600x900 capture of GENERAL: the three checkbox rows sit
    // at a uniform 34px pitch with identical 8px gaps. The `_y += 10` and `_y += 6` between them
    // would have made those gaps 18 and 14.
    //
    // They are reintroduced here at ZERO so the render cannot move, and so that giving a row real
    // space is now one argument at the call site instead of a statement that looks like it works.
    private readonly List<(Control C, int H, bool Wide, int GapBelow)> _rows = new();

    /// <summary>
    /// The width of a full-width row, measured from the panel instead of assumed.
    ///
    /// The vertical scrollbar's width comes off whether or not that scrollbar is showing. A row
    /// sized to the full client width fits exactly right up until the page grows tall enough to
    /// scroll, at which point the client area narrows by one scrollbar and every row is suddenly
    /// too wide - which the user sees as a horizontal scrollbar appearing under content that fitted
    /// a moment earlier.
    /// </summary>
    private int RowWidth
    {
        get
        {
            // MEASURE THE VIEWPORT, DO NOT COMPUTE IT. This used to be
            // `_content.Width - SystemInformation.VerticalScrollBarWidth`, which is the outer width
            // minus a guess, while AutoScroll decides using ClientSize. The two agree only when the
            // guess is exactly right, and when they disagree by even one pixel AutoScroll adds a
            // horizontal bar AND scrolls the pane - which is what cut the first character off every
            // label ("nstalled to:", "Jninstalling").
            //
            // ClientSize already excludes the vertical scrollbar WHEN ONE IS SHOWN. When one is not,
            // reserve it, because the page can grow tall enough to need it after this row is sized
            // and everything would then be one scrollbar too wide.
            var w = _content.ClientSize.Width;
            if (w <= 0)
            {
                w = _content.Width;     // before the first layout pass ClientSize is meaningless
            }
            if (!_content.VerticalScroll.Visible)
            {
                w -= SystemInformation.VerticalScrollBarWidth;
            }
            return Math.Max(160, w - RowSafetyMargin);
        }
    }

    /// <summary>
    /// A hair off the viewport. Third-party controls do not all honour an assigned Width exactly -
    /// MetroCheckBox paints its own frame - and being one pixel under costs nothing visible, while
    /// being one pixel over costs a horizontal scrollbar across the whole screen.
    /// </summary>
    private const int RowSafetyMargin = 2;

    private bool _laying;

    /// <summary>
    /// Re-width every stretchy row, then re-stack every row vertically.
    ///
    /// The second half is new and is what makes a resize actually fit: the GamePreview keeps its
    /// screenshot's aspect ratio, so widening the window makes it TALLER, and the rows recorded
    /// below it at build time would then sit on top of it. Asking each row for its current height
    /// instead of replaying the recorded one keeps the stack honest whatever a row decides to become.
    ///
    /// Re-entrancy guard: assigning Width to a GamePreview raises its SizeChanged, which is fine,
    /// but assigning heights can also nudge _content's scrollbar in or out, and that raises
    /// _content.SizeChanged - which calls straight back into here. One pass is enough.
    /// </summary>
    private void LayoutRows()
    {
        if (_laying)
        {
            return;
        }
        _laying = true;
        try
        {
            var w = RowWidth;
            foreach (var c in _wide)
            {
                c.Width = w;
            }

            var y = 0;
            foreach (var (c, h, _, gap) in _rows)
            {
                c.Location = new Point(0, y);
                // A row whose height is derived from its width reports the truth through Height; a
                // fixed row keeps the height it was built with, because its Height may legitimately
                // differ from its row slot (the slot includes the gap under it).
                //
                // GeneralsX @refactor Claude 17/08/2026 Tested through IReflowsToWidth rather than
                // against one control by name. Three controls now derive their height from their
                // width (GamePreview, HudAnchorPreview, and the changelog's notes panel), and a type
                // test that named one of them would have let the others overlap the rows beneath
                // them - a fault that paints like a rendering bug and reads nothing like the missing
                // case in a stacker that it actually is.
                y += (c is IReflowsToWidth ? c.Height + 8 : h) + gap;
            }
        }
        finally
        {
            _laying = false;
        }
    }

    private Label? _debugWarn;

    /// <summary>
    /// The debug warning changes colour and wording with the setting, rather than sitting there as
    /// permanent grey small print that nobody reads. When it is ON it is the loudest thing on the
    /// page, because someone who forgets they left it on will be told "you cannot join" by a
    /// network error that does not mention debug mode at all.
    /// </summary>
    private void RefreshDebugWarning()
    {
        if (_debugWarn is null)
        {
            return;
        }
        if (_s.DebugMode)
        {
            _debugWarn.ForeColor = Color.FromArgb(0xE8, 0xB3, 0x39);
            _debugWarn.Text =
                "DEBUG MODE IS ON — you can only play against AI.\n" +
                "Online and LAN games are unavailable while this is on. A debug client does not run\n" +
                "the same simulation as everyone else, and this game is lockstep: every player must\n" +
                "simulate identically or the match desyncs. Turn this off to play with other people.";
        }
        else
        {
            _debugWarn.ForeColor = LauncherTheme.TextDim;
            _debugWarn.Text =
                "Debug mode adds asserts and verbose logging for diagnosing a problem.\n" +
                "It restricts you to skirmish against AI, because a debug client cannot stay in step\n" +
                "with other players.";
        }
    }

    /// <summary>
    /// Stack one control under the last. A <paramref name="wide"/> row is stretched to the panel by
    /// LayoutRows; anything else keeps the size it was given, because a spin box or a button
    /// stretched across the whole pane is not a control, it is a mistake.
    /// </summary>
    private void Row(Control c, int height = 34, bool wide = true, int gapBelow = 0)
    {
        // Location is provisional: LayoutRows overwrites it on the very next call, and Select()
        // always makes that call. It is set at all only so a row is not painted at 0,0 first.
        c.Location = new Point(0, 0);
        if (wide)
        {
            _wide.Add(c);
        }
        // GeneralsX @feature Claude 16/08/2026 Rows are recorded IN ORDER with the height they were
        // given, so LayoutRows can re-stack them vertically as well as re-width them. It used to only
        // re-width, which was correct while every row's height was a constant - and stopped being
        // correct the moment the preview's height started depending on the window width.
        _rows.Add((c, height, wide, gapBelow));
        _content.Controls.Add(c);
    }

    private void Note(string text)
    {
        Row(new Label
        {
            Text = text, AutoSize = false, Size = new Size(RowWidth, 22), UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9f),
            TextAlign = ContentAlignment.MiddleLeft,
        }, 26);
    }

    /// <summary>
    /// GxCheckBox rather than a plain WinForms CheckBox, because the stock one paints a grey Win32
    /// box in the middle of a dark page. It was MetroCheckBox until 19/08/2026; that control could
    /// not be made DPI-correct, which is what cost the whole dependency. See GxCheckBox.
    ///
    /// Return type stays CheckBox - GxCheckBox derives from it - so every caller and all the
    /// CheckedChanged wiring is unchanged by the swap.
    /// </summary>
    private CheckBox Check(string label, bool value, Action<bool> set)
    {
        var c = new GxCheckBox
        {
            Text = label, Checked = value, AutoSize = false, Size = new Size(RowWidth, 26),
            UseMnemonic = false,
        };
        c.CheckedChanged += (_, _) => set(c.Checked);
        return c;
    }

    // GeneralsX @feature wh1ter0se 20/08/2026 Rebuilt against design/OptionsGeneral.dc.html.
    //
    // Order is the design's and it is deliberate: the paths first (what this install IS), then the
    // archive you cannot switch off, then the ones you can. The shortcut checkboxes moved to the
    // BOTTOM rather than being dropped - the design does not show them, but they are live settings
    // and silently removing a control people use is worse than a page that carries one extra group.
    private void BuildGeneral()
    {
        // _patchDir is normally assigned inside BuildOptionalContent, which now runs AFTER the
        // required list needs it. Resolve it up front instead.
        _patchDir = Program.SnapMode && Program.SnapFixtureDir is not null
            ? Program.SnapFixtureDir
            : Path.Combine(OwnedPaths.InstallDir, "GeneralsXPatch");

        BuildGeneralPaths();
        BuildRequiredArchives();

        SectionHead("Optional, and safe to change");
        Note("Applied immediately. No save needed.");
        BuildOptionalContent();
        BuildIndicatorSwitch();

        // Accurate, and deliberately not the artboard's "All of these change your data ID" - see
        // the header comment in GeneralPage.cs for the measurement that contradicts it.
        Note("Only the required set above affects who you can play with.");

        SectionHead("Shortcuts");
        Row(Check("Create a Desktop shortcut", _s.DesktopShortcut, v => _s.DesktopShortcut = v));
        Row(Check("Create a Start Menu shortcut", _s.StartMenuShortcut, v => _s.StartMenuShortcut = v));
        Row(Check("Close the launcher after the game starts", _s.CloseOnPlay, v => _s.CloseOnPlay = v));
        Note("Uninstalling is deleting the install folder. Nothing is written to the registry.");
    }

    /// <summary>
    /// Optional game-data archives, toggled by renaming them in GeneralsXPatch.
    ///
    /// WHY A RENAME AND NOT A SETTING. The engine mounts every *.big it finds in that folder and
    /// there is no switch to tell it otherwise, so the only thing that can turn one off is its
    /// name. It ships as &lt;name&gt;.big.off and becomes &lt;name&gt;.big when ticked.
    /// SearchStringMatches requires the pattern and the name to end together (ArchiveFile.cpp),
    /// so "*.big" does not match "x.big.off". A subdirectory would NOT work - the overlay scan
    /// passes searchSubdirectories=TRUE and would mount a "disabled" folder anyway.
    ///
    /// WHY THE LIST IS READ FROM DISK RATHER THAN COMPILED IN. optional.txt ships beside the
    /// archives, written by package-windows-share.ps1 from the repo's overlay-optional.txt. A
    /// compiled-in list would drift from the payload the first time the set changed, and the
    /// launcher would then offer a checkbox for an archive that is not there - or, far worse,
    /// offer to switch off one that IS mandatory.
    ///
    /// NOTHING HERE CAN COST SOMEBODY A MATCH. Every archive on that list is measured to contain
    /// no file the SimID fingerprint reads, so ticking a box does not move dataID and cannot make
    /// this install refuse its friends. That property is enforced where the list is written, not
    /// here - this code trusts the manifest, and the manifest is the thing under test.
    ///
    /// THE DEFAULT IS DATA TOO, AND FOR THE SAME REASON THE LIST IS. optional.txt carries a third
    /// field, "on" or "off", copied straight from overlay-optional.txt by the packager. Compiling
    /// the defaults in would be worse than compiling the names in: a wrong NAME shows up instantly
    /// as a checkbox that toggles nothing, while a wrong DEFAULT is invisible - the box works, and
    /// the only thing that is wrong is the sentence telling the player what shipped as standard,
    /// which is precisely the fact they cannot check for themselves.
    /// </summary>
    private void BuildOptionalContent()
    {
        // Under --snap this points at a throwaway fixture instead of the install, so the optional
        // section and its picture are actually built. See Program.SnapFixtureDir.
        _patchDir = Program.SnapMode && Program.SnapFixtureDir is not null
            ? Program.SnapFixtureDir
            : Path.Combine(OwnedPaths.InstallDir, "GeneralsXPatch");
        string listPath = Path.Combine(_patchDir, "optional.txt");

        // A payload predating this feature simply has no manifest, and that is not an error -
        // the section just does not appear. Same for an install somebody has emptied.
        List<OptionalEntry> entries;
        try
        {
            entries = File.Exists(listPath)
                ? OptionalList.Parse(File.ReadAllLines(listPath))
                : new List<OptionalEntry>();
        }
        catch (IOException)
        {
            return;             // unreadable manifest: show nothing rather than a broken section
        }

        // Neither name present means the payload is incomplete for that entry, and offering a
        // checkbox that cannot do anything is worse than leaving it out. Filtered HERE rather than
        // skipped inside the row loop, because the picture and its caption both have to know the
        // real set before the first row is built.
        //
        // A NAME FROM THIS FILE BECOMES A PATH AND THEN AN ARGUMENT TO File.Move, so it is required
        // to be a bare leaf name first. This is not a privilege boundary - the manifest lives inside
        // an install the user already owns - but "GeneralsXPatch/optional.txt says so" is not a good
        // enough reason to rename a file somewhere else on the disk, and a one-line check is cheaper
        // than the argument about whether it could ever matter.
        _optional = new List<OptionalEntry>();
        foreach (var e in entries)
        {
            if (e.Name.Length == 0 || Path.GetFileName(e.Name) != e.Name)
            {
                continue;
            }
            if (File.Exists(OnPath(e)) || File.Exists(OffPath(e)))
            {
                _optional.Add(e);
            }
        }
        if (_optional.Count == 0) return;

        // ONE PICTURE, ABOVE THE BOXES THAT CHANGE IT.
        //
        // GeneralsX @feature Claude 17/08/2026 This replaces one framed preview box per option -
        // five of them, each with its own placeholder, and on any machine that could not reach the
        // CDN five copies of "Preview unavailable offline". The owner's instruction was "one
        // picture that changes dynamically for all", and it is right for a reason beyond taste:
        // the thing a player is choosing is not five independent options, it is the single screen
        // they will be looking at, and five separate pictures cannot show how the choices combine.
        //
        // It sits ABOVE the checkboxes rather than below them so that the eye lands on the result
        // first and the controls read as the thing that changes it.
        _art = PreviewArtSource.Embedded();

        // HAS THE PICTURE ROTTED? The optional-content tier and this launcher ship on separate tiers
        // and update independently, so a rebuilt tier leaves the embedded screenshot showing bytes
        // nobody has any more. The base names the content-addressed artifact it was captured from,
        // gxinstalled.txt names the ones actually installed, and a mismatch is captioned rather than
        // hidden - the same treatment the fetched previews gave a stale shot, which embedding the
        // art had quietly dropped.
        _artStale = !PreviewSource.ArtifactMatchesInstall(
            _art.Artifact, PreviewSource.InstalledArtifacts(ReadInstalledLines(OwnedPaths.InstallDir)));

        _gamePreview = new GamePreview(RowWidth);
        _gamePreview.SetArt(_art, _optional.Select(e => e.Name));
        Row(_gamePreview, _gamePreview.Height + 8, wide: true);

        foreach (var e in _optional)
        {
            string on = OnPath(e), off = OffPath(e);
            bool enabled = File.Exists(on);

            // BOTH NAMES ON DISK IS A REAL STATE AND THE BOX MUST NOT PRETEND OTHERWISE. It is what
            // flipping a default from on to off currently produces - the payload stages
            // "<name>.big.off", a new filename, so the old "<name>.big" is neither renamed nor
            // removed and the engine keeps mounting it (see overlay-optional.txt). The toggle reads
            // its state as File.Exists(on), so the box shows TICKED, and SetOptionalArchive only
            // moves a file when exactly one of the pair exists, so ticking and unticking do nothing
            // at all. Fixing that is a packaging change and is not attempted here; what IS fixed is
            // the lie - the box is disabled and the row says what is actually happening, instead of
            // snapping back and leaving the player certain the launcher is broken.
            bool stuck = enabled && File.Exists(off);

            // The box is put back to what the DISK says, not to what was clicked. A running game
            // can hold the archive open, and a tick that claims a change the filesystem refused
            // is how somebody ends up certain they turned something off. `suppress` is per-entry
            // (a fresh local each iteration) and stops the corrective assignment re-entering the
            // handler it is being made from.
            CheckBox? box = null;
            bool suppress = false;
            box = Check(e.Label, enabled, v =>
            {
                if (suppress) return;
                SetOptionalArchive(on, off, v);
                bool actual = File.Exists(on);
                if (actual != v && box != null)
                {
                    suppress = true;
                    box.Checked = actual;
                    suppress = false;
                }
                // The picture is what the whole section is for, so it is repainted from the DISK
                // state after every toggle rather than from the checkbox that was just clicked.
                RefreshOptional();
            });
            box.Enabled = !stuck;
            Row(box);

            // The default, whether it has been changed, and whether the picture above depicts it.
            // Under the box rather than in a tooltip, because a fact a player has to hover to
            // discover is a fact they will not discover.
            //
            // A NotesPanel rather than a Label, so the row's HEIGHT comes from the measured text.
            // Cross-model review caught the fixed-height version clipping the one message that
            // actually needs reading: the both-files-installed explanation is two sentences, which
            // is three or four wrapped lines at the launcher's minimum width, and a 34px label
            // showed the first of them and swallowed the remedy. Being IReflowsToWidth is what makes
            // LayoutRows ask each row for its current height instead of replaying a recorded one.
            var status = new NotesPanel(8.5f);
            Row(status, 38);
            _optionRows.Add((e, box, status));
        }

        RefreshOptional();

        Note("    Takes effect the next time the game starts. Everyone can play together");
        Note("    whichever way these are set.");

        _ = FillPreviewGapsAsync(_gamePreview, _art, _optional.Select(e => e.Name).ToList());
    }

    private string OnPath(OptionalEntry e) => Path.Combine(_patchDir, e.Name);

    /// <summary>
    /// The switched-off spelling. Kept next to OnPath so the ".off" suffix appears once: it is the
    /// engine's rule, not a preference - SearchStringMatches requires the pattern and the name to
    /// terminate together (ArchiveFile.cpp:71-80), which is exactly why "*.big" does not match
    /// "x.big.off".
    /// </summary>
    private string OffPath(OptionalEntry e) => OnPath(e) + ".off";

    /// <summary>
    /// Repaint the picture and rewrite every row's status line from what is ACTUALLY ON DISK.
    ///
    /// One function for both, deliberately: the caption's "not shown in this picture" list and the
    /// rows' "no preview available" markers are the same verdict said twice, and computing them in
    /// two places is how they would eventually disagree in front of a player.
    /// </summary>
    private void RefreshOptional()
    {
        if (_gamePreview is null)
        {
            return;
        }

        var enabled = new List<string>();
        var notShown = new List<string>();

        foreach (var (e, _, status) in _optionRows)
        {
            bool on = File.Exists(OnPath(e));
            bool stuck = on && File.Exists(OffPath(e));
            if (on)
            {
                enabled.Add(e.Name);
            }

            var layer = _art.For(e.Name);
            var how = layer.How;
            if (how == PreviewDepiction.Layer && !_gamePreview.HasLayer(e.Name))
            {
                // The manifest named art the picture refused - it did not decode, or it is not the
                // base's size. Demoted to "no preview available" rather than claimed, because the
                // only thing worse than a missing picture is a caption about one.
                how = PreviewDepiction.None;
            }

            // WHAT "DEPICTED" MEANS, PER CASE, and why the base case is the asymmetric one:
            //  * Silent - the option provably changes nothing a shot like this can show, so the
            //    picture is right whichever way it is set.
            //  * Layer  - the base was captured WITHOUT it (a key may be named by 'shows' or by
            //    'layer', never both), so off is the base and on is the base plus the layer. Both
            //    states are depicted.
            //  * Base   - the base was captured WITH it. On is depicted; off is NOT, because there
            //    is no screenshot of the game without it and nothing here can invent one.
            //  * None   - no art and no claim. Not depicted in either state: we do not know what
            //    the base was captured with for this option, so neither state can be asserted.
            bool depicted = how switch
            {
                PreviewDepiction.Silent => true,
                PreviewDepiction.Layer => true,
                PreviewDepiction.Base => on,
                _ => false,
            };
            if (!depicted)
            {
                notShown.Add(e.Label);
            }

            status.SetLines(StatusLines(e, on, stuck, how, layer.Why));
        }

        _gamePreview.Compose(enabled, notShown, _artStale);

        // Compose re-measures the picture's captions, so the picture's HEIGHT can change with the
        // selection - a fourth option dropping onto a second "not shown" line is enough. Re-stack,
        // or the rows underneath sit on top of it. LayoutRows guards its own re-entry.
        LayoutRows();
    }

    /// <summary>
    /// One row's plain-language status: what shipped, whether it has been changed, and what the
    /// picture above is and is not showing about it.
    ///
    /// EVERY NAME AND EVERY DEFAULT IN HERE COMES FROM DATA. The entry is a line of
    /// GeneralsXPatch/optional.txt, which the packager writes from the repo's overlay-optional.txt;
    /// nothing about which archives exist or how they ship is compiled into this program, so the
    /// two cannot drift. "Default not recorded" is what an older payload gets - a payload written
    /// before the default was carried through simply does not say, and inventing "off" for it would
    /// be a confident guess about the one thing this line exists to state.
    /// </summary>
    private static List<NotesPanel.Line> StatusLines(OptionalEntry e, bool on, bool stuck,
        PreviewDepiction how, string why)
    {
        var parts = new List<string>
        {
            e.DefaultOn switch
            {
                true => "On by default",
                false => "Off by default",
                _ => "Default not recorded by this payload",
            },
        };

        if (e.DefaultOn is { } shipped && shipped != on)
        {
            parts.Add(on ? "you switched it on" : "you switched it off");
        }

        parts.Add(how switch
        {
            PreviewDepiction.Base => on
                ? "shown in the picture above"
                : "the picture above was captured with this on, so it does not show it switched off",
            PreviewDepiction.Layer => on
                ? "shown in the picture above"
                : "the picture above shows it switched off",
            PreviewDepiction.Silent => "no preview needed - " + why,
            _ => "no preview available for this option",
        });

        var lines = new List<NotesPanel.Line>
        {
            new(string.Join("  ·  ", parts) + ".", NotesPanel.Tone.Dim),
        };

        // A SEPARATE, LOUDER PARAGRAPH rather than a second sentence on the same quiet line. This is
        // the only text on the page that asks the player to go and do something, and it appears in a
        // state where the control next to it is deliberately dead - so it has to be the thing that
        // is read, not small print beside a greyed-out box.
        if (stuck)
        {
            lines.Add(new NotesPanel.Line(
                "Both " + e.Name + " and " + e.Name + ".off are installed, so the game loads this " +
                "whichever way the box is set and the box cannot change it. Delete one of the two " +
                "files in GeneralsXPatch, or reinstall.", NotesPanel.Tone.Caution));
        }
        return lines;
    }

    /// <summary>
    /// The preview document, fetched once per process.
    ///
    /// Static because OptionsPanel rebuilds its page whenever a category is selected, and
    /// re-fetching a document on every click of GRAPHICS would be rude to the CDN and slow for the
    /// user. Null means "not asked yet"; an empty set means "asked, and there is nothing" - the
    /// distinction is what stops a failed fetch being retried on every single page build.
    /// </summary>
    private static PreviewSet? s_previews;
    private static readonly SemaphoreSlim s_previewLock = new(1, 1);

    /// <summary>
    /// The network's remaining job: fill in a layer for an option this build shipped no art for.
    ///
    /// STRICTLY ADDITIVE, AND THAT IS THE WHOLE CONTRACT. It is offered only keys the embedded
    /// manifest makes no claim about at all, and PreviewArt.Add refuses to overwrite an existing
    /// entry - so a fetched document can never replace, contradict or degrade a picture that
    /// already works offline. What it buys is the property Previews.cs was originally built for and
    /// that the owner's rule about relays states generally: a preview for a newly shipped archive
    /// is a publish, not a client release.
    ///
    /// THREE REFUSALS, all of which leave the option reading "no preview available", and all three
    /// applied to the DOCUMENT before a byte is downloaded:
    ///  * A shot that is not a .png. A composited layer has to be a transparent delta, and JPEG has
    ///    no alpha channel at all - so a .jpg here is, by construction, a full opaque frame. Accepted
    ///    as a layer it would blot out the base and every layer under it, and with two such options
    ///    enabled only the higher-precedence one would survive: the picture would then show the
    ///    other option in whatever state that capture happened to have. Cross-model review found
    ///    this; GamePreview refuses a layer with no alpha channel as the matching second gate, so
    ///    an opaque 24-bit PNG does not slip through either.
    ///  * A shot whose declared size is not the base's exactly. Layers are composited, never
    ///    scaled, so a shot of another size cannot go on top of this base without moving the art to
    ///    somewhere it will not be.
    ///  * A shot whose content-addressed artifact is not among the installed ones. The old
    ///    per-option strip could show such a picture with a caution caption beside it, because each
    ///    picture had its own caption. A composed image has ONE caption for the whole stack, so a
    ///    stale layer inside it could not be labelled without labelling everything - and an
    ///    unlabelled stale layer is the confidently-wrong failure this design exists to avoid.
    /// </summary>
    private async Task FillPreviewGapsAsync(GamePreview preview, PreviewArt art, List<string> keys)
    {
        try
        {
            // Read before the first await, while this is still the UI thread.
            var canvas = preview.CanvasSize;
            if (canvas.IsEmpty)
            {
                return;             // no base to layer onto
            }

            var gaps = keys.Where(k => art.For(k).How == PreviewDepiction.None).ToList();
            if (gaps.Count == 0)
            {
                return;
            }

            var dir = OwnedPaths.InstallDir;

            // One fetch for the whole process. Without the gate, two page builds race.
            if (s_previews is null)
            {
                await s_previewLock.WaitAsync().ConfigureAwait(false);
                try
                {
                    s_previews ??= await PreviewSource.LoadAsync(dir).ConfigureAwait(false);
                }
                finally
                {
                    s_previewLock.Release();
                }
            }

            var set = s_previews;
            if (set is null || !set.Any)
            {
                return;
            }

            var installed = PreviewSource.InstalledArtifacts(ReadInstalledLines(dir));
            var found = new List<PreviewLayer>();
            foreach (var key in gaps)
            {
                var shot = set.For(key)?.Shots.FirstOrDefault(
                    s => s.File.EndsWith(".png", StringComparison.OrdinalIgnoreCase) &&
                         s.Width == canvas.Width && s.Height == canvas.Height &&
                         PreviewSource.ShotMatchesInstall(s, installed));
                if (shot is null)
                {
                    continue;
                }
                var data = await PreviewSource.LoadShotAsync(dir, set, shot).ConfigureAwait(false);
                if (data is not null)
                {
                    found.Add(new PreviewLayer(key, PreviewDepiction.Layer, data, ""));
                }
            }
            if (found.Count == 0)
            {
                return;
            }

            // BACK ON THE UI THREAD BEFORE TOUCHING THE ART. PreviewArt holds a plain Dictionary and
            // the UI thread reads it on every toggle; adding to it from here would be a data race
            // whose symptom is an exception inside a click handler.
            if (preview.IsDisposed || !preview.IsHandleCreated)
            {
                return;
            }
            preview.BeginInvoke(() =>
            {
                // The user may have changed page - and Select disposes the old picture - between
                // the check above and this running.
                if (preview.IsDisposed || !ReferenceEquals(_gamePreview, preview))
                {
                    return;
                }
                foreach (var l in found)
                {
                    art.Add(l);
                }
                preview.SetArt(art, keys);
                RefreshOptional();
                LayoutRows();       // the picture's height can change with a new layer set
            });
        }
        catch (Exception)
        {
            // Including the case where the panel was disposed while this was in flight. A missing
            // screenshot must never block a settings page, let alone a launch.
        }
    }

    private static IEnumerable<string> ReadInstalledLines(string dir)
    {
        try
        {
            var f = Path.Combine(dir, "gxinstalled.txt");
            return File.Exists(f) ? File.ReadAllLines(f) : Array.Empty<string>();
        }
        catch (IOException)
        {
            return Array.Empty<string>();
        }
    }

    /// <summary>
    /// Rename one optional archive into or out of the mount. The caller re-reads the disk
    /// afterwards and corrects the checkbox, so a refused rename shows as an untouched box
    /// rather than as a change that did not happen.
    /// </summary>
    private void SetOptionalArchive(string on, string off, bool enable)
    {
        try
        {
            if (enable && !File.Exists(on) && File.Exists(off)) File.Move(off, on);
            else if (!enable && File.Exists(on) && !File.Exists(off)) File.Move(on, off);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    /// <summary>
    /// The resolution the user picked and has not saved yet. Null means they never touched the
    /// picker, and in that case nothing at all is written to the game's Options.ini.
    /// </summary>
    private Size? _resolution;

    /// <summary>
    /// Pending DebugMenu choice, null when the box has not been touched this session. Same
    /// write-on-SAVE-only discipline as _resolution, and cleared on a successful write for the same
    /// reason: this panel outlives its own save, so a choice left set here would be re-applied by
    /// every later SAVE and could silently undo something changed in-game meanwhile.
    /// </summary>
    private bool? _debugMenu;

    /// <summary>
    /// Pending camera values, null until the user touches a camera control. Same write-on-change
    /// discipline as _resolution: nothing is written to SagePatch.ini unless one of these is set.
    /// </summary>
    private int? _minHeight;
    private int? _movePct;

    private void BuildGraphics()
    {
        Row(Check("Windowed", _s.Windowed, v => _s.Windowed = v));
        Row(Check("Cap the frame rate", _s.CapFps, v => _s.CapFps = v));

        var fps = new NumericUpDown
        {
            // 1000, not 480. NumericUpDown CLAMPS a typed value to Maximum silently on commit, so
            // the old ceiling turned "495" into "480" with nothing on screen to say why - it reads
            // as the box rounding, which it never does. The cap is the RENDER rate and the engine
            // takes it verbatim through -fps, so there is no reason for a low ceiling here.
            Minimum = 30, Maximum = 1000, Value = Math.Clamp(_s.FpsCap, 30, 1000),
            Size = new Size(110, 26), BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            // Fully qualified. It had to be while MetroForm declared its own BorderStyle member and
            // shadowed the bare name; that is gone with the dependency, but the qualification is
            // kept because it costs nothing and states which BorderStyle is meant.
            BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle,
        };
        fps.ValueChanged += (_, _) => _s.FpsCap = (int)fps.Value;
        Row(fps, 34, wide: false);

        Note("Resolution");

        // Read from and written back to the GAME's Options.ini, not to gxlauncher.ini, because that
        // is the only file the engine looks in: OptionPreferences::getResolution parses the key
        // "Resolution" with sscanf("%d%d"). Deliberately NOT mirrored into gxlauncher.ini as well -
        // the game rewrites Options.ini every time it exits, so a launcher-side copy would go stale
        // the first time somebody changed the resolution in-game and would then silently undo their
        // change on the next save.
        var current = GameOptions.ReadResolution();
        var modes = GameOptions.Modes(_resolution ?? current);
        var combo = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList, FlatStyle = FlatStyle.Flat,
            Size = new Size(200, 26), Font = new Font("Segoe UI", 10f),
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
        };
        foreach (var m in modes)
        {
            combo.Items.Add($"{m.Width} x {m.Height}");
        }
        var shown = _resolution ?? current;
        combo.SelectedIndex = shown is { } s ? modes.IndexOf(s) : -1;
        // Wired AFTER the initial selection, so filling the list is not mistaken for a choice and
        // does not arm the writer above.
        combo.SelectedIndexChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0)
            {
                _resolution = modes[combo.SelectedIndex];
            }
        };
        Row(combo, 34, wide: false);

        Note("The resolution is applied to the game itself and takes effect the next time it starts.");
        Note("The frame cap is the RENDER rate. It does not change the simulation rate, which is");
        Note("shared by every player in the match and set by the slowest peer.");

        Note("Camera");

        var cam = GameSagePatch.ReadCamera();

        Note("Minimum camera height  (how far in you can zoom)");
        var minH = new NumericUpDown
        {
            Minimum = 50, Maximum = 300, Increment = 5,
            Value = Math.Clamp(_minHeight ?? cam.MinHeight, 50, 300),
            Size = new Size(110, 26), BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle,
        };
        minH.ValueChanged += (_, _) => _minHeight = (int)minH.Value;
        Row(minH, 34, wide: false);

        Note("Camera movement speed  (keyboard scrolling), %");
        var speed = new NumericUpDown
        {
            Minimum = 50, Maximum = 300, Increment = 10,
            Value = Math.Clamp(_movePct ?? cam.MovePct, 50, 300),
            Size = new Size(110, 26), BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle,
        };
        speed.ValueChanged += (_, _) => _movePct = (int)speed.Value;
        Row(speed, 34, wide: false);

        var restore = new BigButton("RESTORE DEFAULTS", LauncherTheme.Bg, LauncherTheme.Border)
        {
            Ghost = true, Size = new Size(200, 36),
        };
        restore.Click += (_, _) =>
        {
            minH.Value = GameSagePatch.DefaultMinHeight;
            speed.Value = GameSagePatch.DefaultMovePct;
            // Set the pending values EXPLICITLY rather than relying on ValueChanged: assigning a value
            // a box already holds raises no event, so restoring an already-default screen would mark
            // nothing pending and SAVE would write nothing at all.
            _minHeight = GameSagePatch.DefaultMinHeight;
            _movePct = GameSagePatch.DefaultMovePct;
        };
        Row(restore, 44, wide: false);

        Note($"Defaults match Generals Online: minimum {GameSagePatch.DefaultMinHeight}, movement speed {GameSagePatch.DefaultMovePct}%.");
        Note("Camera settings take effect the next time the game starts. Maximum zoom-out is fixed at");
        Note("310 for everybody, so nobody gets a see-further advantage; speed here is keyboard scroll.");
    }

    private void BuildNetwork()
    {
        // READ ONLY, on purpose. Discovery is the launcher's job.
        Note("Server           discovered automatically");
        Note("Relay list       fetched and signature-checked on every launch");
        Note("There is nothing to configure here. Earlier builds asked you to paste a server");
        Note("address; the launcher now finds it, verifies who signed it, and picks the closest.");

        // GeneralsX @feature Claude 17/08/2026 The test section. Read-only discovery above answers
        // "what is configured"; this answers "why can't I join my friend", which is the question
        // that actually brings somebody to this page. Built in NetworkPage.cs; the decisions it
        // renders are in NetworkProbe.cs, which ../loader-tests compiles and runs.
        BuildNetworkTest();
    }

    private void BuildDiagnostics()
    {
        Row(Check("Keep a launcher log", _s.KeepLog, v => _s.KeepLog = v));
        Row(Check("Show the in-game network overlay (ping, frame time)", _s.NetOverlay, v => _s.NetOverlay = v));

        // DEBUG MODE, and the warning is not boilerplate - it is the reason the option can exist.
        //
        // This engine is LOCKSTEP: every peer simulates the same frames from the same inputs, and a
        // peer whose simulation diverges by one unit of anything desyncs the whole match. Debug
        // builds add asserts, extra logging and different code paths, so a debug peer is not the
        // same simulation as everybody else's. On top of that, joining is gated on sourceID, a
        // digest of the simulation source - a debug client would be refused by the other peers
        // anyway, and the refusal reads as a confusing network error rather than an explanation.
        //
        // So the honest framing is: this is a single-player tool. Turn it on to diagnose something
        // against AI; turn it off to play with people.
        // THE GAME'S FILE IS THE TRUTH, not our remembered bool - the engine reads DebugMenu out of
        // Options.ini and has never seen gxlauncher.ini. Seeding the box from _s.DebugMode alone
        // would show ticked while the game had it off, which is precisely the state this bug left
        // every existing install in.
        //
        // MIGRATION, and it is the case that actually matters: anyone who ticked this box before the
        // wiring existed has DebugMode=1 remembered and no DebugMenu key at all. Their intent was
        // recorded and never delivered. Treating that disagreement as a pending change means one
        // SAVE reconciles it, instead of requiring them to untick and retick a box that already
        // looks correct.
        var debugNow = GameOptions.ReadDebugMenu();
        if (_s.DebugMode != debugNow)
        {
            _debugMenu = _s.DebugMode;
            debugNow = _s.DebugMode;
        }
        var dbg = Check("Enable debug mode  (single-player only)", debugNow, v =>
        {
            _s.DebugMode = v;
            // Mirrors the resolution picker: remember the choice, let SAVE write the game's file.
            // Deliberately NOT written here - the game's Options.ini is shared with retail Zero Hour
            // and the Generals Online client, and merely toggling a checkbox must not touch it.
            _debugMenu = v;
            RefreshDebugWarning();
        });
        Row(dbg);

        _debugWarn = new Label
        {
            AutoSize = false, Size = new Size(RowWidth, 76), UseMnemonic = false,
            Font = new Font("Segoe UI", 9f), TextAlign = ContentAlignment.TopLeft,
        };
        Row(_debugWarn, 84);
        RefreshDebugWarning();


        var open = new BigButton("OPEN LOG FOLDER", LauncherTheme.Bg, LauncherTheme.Border)
        {
            Ghost = true, Size = new Size(220, 42),
        };
        open.Click += (_, _) =>
        {
            try
            {
                Directory.CreateDirectory(OwnedPaths.InstallDir);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(OwnedPaths.InstallDir)
                {
                    UseShellExecute = true,
                });
            }
            catch
            {
                // Explorer refusing to open is not worth a dialog.
            }
        };
        Row(open, 50, wide: false);
        Note("Send that folder's gxlaunch.log when reporting a problem.");
    }

    private void BuildAbout()
    {
        Note("GeneralsX");
        Note("Command & Conquer: Generals Zero Hour on Windows, macOS, Linux and iPadOS,");
        Note("running the original simulation - one lockstep match across all four.");
        Note("This launcher never modifies your Zero Hour installation. It only reads from it.");
        Note("You must own the game.");
    }
}

/// <summary>
/// Launcher preferences, in gxlauncher.ini beside the executable.
///
/// Hand-rolled key=value rather than a serializer: this file is meant to be readable and fixable by
/// a person over chat when something goes wrong, and it must survive a partially written line
/// rather than throwing on load.
/// </summary>
internal sealed class LauncherSettings
{
    public bool DesktopShortcut { get; set; } = true;
    public bool StartMenuShortcut { get; set; } = true;
    public bool CloseOnPlay { get; set; } = true;

    /// <summary>
    /// DEFAULTS ON, and that is not a preference, it is the behaviour that already ships. Until this
    /// setting was wired up, GeneralsX.cmd passed "-win" unconditionally, so every install has always
    /// run windowed while this checkbox sat unticked - the screen was describing a state it did not
    /// control. Defaulting to off would have turned wiring the setting up into a silent "the update
    /// put my game fullscreen" for everybody who never opened this page.
    /// </summary>
    public bool Windowed { get; set; } = true;

    public bool CapFps { get; set; }
    public int FpsCap { get; set; } = 120;
    public bool KeepLog { get; set; } = true;
    public bool NetOverlay { get; set; }

    /// <summary>Single-player only - see RefreshDebugWarning for why.</summary>
    public bool DebugMode { get; set; }

    /// <summary>
    /// An explicit Zero Hour folder, set from the GENERAL page's CHANGE button.
    ///
    /// Empty is the normal case and means "detect it", which is what the design promises and what
    /// survives the player moving a Steam library. Only a folder that passed
    /// GameInstall.LooksLikeZeroHour is ever stored here.
    /// </summary>
    public string GameDataDir { get; set; } = "";

    private static string PathOf() => Path.Combine(OwnedPaths.InstallDir, "gxlauncher.ini");

    public static LauncherSettings Load()
    {
        var s = new LauncherSettings();
        try
        {
            var p = PathOf();
            if (!File.Exists(p))
            {
                return s;
            }
            foreach (var raw in File.ReadAllLines(p))
            {
                var line = raw.Trim();
                var eq = line.IndexOf('=');
                if (eq <= 0 || line.StartsWith("#") || line.StartsWith(";"))
                {
                    continue;
                }
                var k = line[..eq].Trim();
                var v = line[(eq + 1)..].Trim();
                switch (k)
                {
                    case nameof(GameDataDir): s.GameDataDir = v; break;
                    case nameof(DesktopShortcut): s.DesktopShortcut = v == "1"; break;
                    case nameof(StartMenuShortcut): s.StartMenuShortcut = v == "1"; break;
                    case nameof(CloseOnPlay): s.CloseOnPlay = v == "1"; break;
                    case nameof(Windowed): s.Windowed = v == "1"; break;
                    case nameof(CapFps): s.CapFps = v == "1"; break;
                    case nameof(KeepLog): s.KeepLog = v == "1"; break;
                    case nameof(NetOverlay): s.NetOverlay = v == "1"; break;
                    case nameof(DebugMode): s.DebugMode = v == "1"; break;
                    case nameof(FpsCap):
                        if (int.TryParse(v, out var n)) { s.FpsCap = n; }
                        break;
                }
            }
        }
        catch
        {
            // A corrupt preferences file must never stop the launcher opening.
        }
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(OwnedPaths.InstallDir);
            var b = new System.Text.StringBuilder();
            b.AppendLine("# GeneralsX launcher preferences. Safe to delete.");
            b.AppendLine($"{nameof(GameDataDir)}={GameDataDir}");
            b.AppendLine($"{nameof(DesktopShortcut)}={(DesktopShortcut ? 1 : 0)}");
            b.AppendLine($"{nameof(StartMenuShortcut)}={(StartMenuShortcut ? 1 : 0)}");
            b.AppendLine($"{nameof(CloseOnPlay)}={(CloseOnPlay ? 1 : 0)}");
            b.AppendLine($"{nameof(Windowed)}={(Windowed ? 1 : 0)}");
            b.AppendLine($"{nameof(CapFps)}={(CapFps ? 1 : 0)}");
            b.AppendLine($"{nameof(FpsCap)}={FpsCap}");
            b.AppendLine($"{nameof(KeepLog)}={(KeepLog ? 1 : 0)}");
            b.AppendLine($"{nameof(NetOverlay)}={(NetOverlay ? 1 : 0)}");
            b.AppendLine($"{nameof(DebugMode)}={(DebugMode ? 1 : 0)}");
            File.WriteAllText(PathOf(), b.ToString(), new System.Text.UTF8Encoding(false));
        }
        catch
        {
            // Losing a preference is not worth an error dialog in front of someone who wants to play.
        }
    }
}

/// <summary>
/// The launcher's window onto the GAME's own Options.ini.
///
/// Separate from LauncherSettings because it is a different file with a different owner. Everything
/// in gxlauncher.ini belongs to this launcher and can be deleted with it; Options.ini belongs to the
/// game and is shared with retail Zero Hour and with the Generals Online client, so the rule here is
/// stricter: read the whole file, drop only the one key being set, append it, and write every other
/// line back unchanged. Never rewrite the file from a model of what we think should be in it - a key
/// this launcher has never heard of is a key some other program is relying on.
///
/// The line format is the engine's own - UserPreferences::write emits "%s = %s\n" and
/// UserPreferences::load trims both sides of the '=' - so what is written here is indistinguishable
/// from what the game writes itself.
/// </summary>
internal static class GameOptions
{
    /// <summary>
    /// Read by OptionPreferences::getResolution with sscanf("%d%d"), so the value is two numbers
    /// separated by whitespace: "1920 1080". Case matters - the engine looks the key up by exact
    /// name.
    /// </summary>
    private const string ResolutionKey = "Resolution";

    /// <summary>
    /// Read by DebugFloatButton.cpp's debugFloatWanted() as prefs.find("DebugMenu") compared with
    /// stricmp against "yes". Case matters on the KEY - PreferenceMap is a std::map keyed by
    /// AsciiString whose operator&lt; is strcmp, and UserPreferences::load stores keys verbatim with no
    /// folding, so "debugmenu" silently never matches. The VALUE is case-insensitive.
    ///
    /// GeneralsX @bugfix Claude 11/08/2026 THIS IS THE KEY THAT WAS MISSING. The checkbox wrote
    /// DebugMode=1 into gxlauncher.ini - our own file - and the engine reads DebugMenu out of the
    /// GAME's Options.ini. Two different keys in two different files with no code between them, so
    /// ticking the box did nothing at all. Exactly the shape the Windowed checkbox and the frame cap
    /// were both in before they were wired up: a control that changed a number in a file nothing
    /// else opened.
    /// </summary>
    private const string DebugMenuKey = "DebugMenu";

    /// <summary>The resolution the game is currently set to, or null if it has never recorded one.</summary>
    public static Size? ReadResolution()
    {
        try
        {
            var p = OwnedPaths.OptionsIni;
            if (!File.Exists(p))
            {
                return null;
            }
            foreach (var line in File.ReadAllLines(p))
            {
                if (!IsKey(line, ResolutionKey))
                {
                    continue;
                }
                var v = line[(line.IndexOf('=') + 1)..];
                var parts = v.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length >= 2 &&
                    int.TryParse(parts[0], out var w) && int.TryParse(parts[1], out var h) &&
                    w > 0 && h > 0)
                {
                    return new Size(w, h);
                }
            }
        }
        catch
        {
            // An unreadable Options.ini means we simply do not know what the game is set to.
        }
        return null;
    }

    /// <summary>
    /// Set Resolution, leaving every other line exactly as it was.
    ///
    /// The old line is filtered out rather than edited in place so that a file which somehow has two
    /// of them ends up with one, and the new line is appended rather than inserted so that no
    /// section ordering the game might care about is disturbed.
    /// </summary>
    public static bool WriteResolution(Size mode)
    {
        try
        {
            var p = OwnedPaths.OptionsIni;
            var dir = Path.GetDirectoryName(p);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var lines = File.Exists(p)
                ? File.ReadAllLines(p).Where(l => !IsKey(l, ResolutionKey)).ToList()
                : new List<string>();

            // Trailing blank lines are dropped before appending, otherwise the file grows a gap
            // every time this runs.
            while (lines.Count > 0 && lines[^1].Trim().Length == 0)
            {
                lines.RemoveAt(lines.Count - 1);
            }

            lines.Add($"{ResolutionKey} = {mode.Width} {mode.Height}");
            IniLine.WriteAllLinesReplacing(p, lines);
            return true;
        }
        catch
        {
            // A read-only Documents folder is somebody else's problem to fix; it must not take the
            // launcher down on the way to a game.
            return false;
        }
    }

    /// <summary>What the game's Options.ini currently says about the debug menu.</summary>
    public static bool ReadDebugMenu()
    {
        try
        {
            var p = OwnedPaths.OptionsIni;
            if (!File.Exists(p))
            {
                return false;
            }
            foreach (var line in File.ReadAllLines(p))
            {
                if (IsKey(line, DebugMenuKey))
                {
                    var v = line[(line.IndexOf('=') + 1)..].Trim();
                    return string.Equals(v, "yes", StringComparison.OrdinalIgnoreCase);
                }
            }
        }
        catch
        {
            // Unreadable means we do not know; treat as off rather than guessing on.
        }
        return false;
    }

    /// <summary>
    /// Set DebugMenu, leaving every other line exactly as it was. Same filter-then-append shape as
    /// WriteResolution, and for the same reasons.
    ///
    /// "no" is written rather than the key being deleted when the box is unticked. The engine only
    /// matches "yes", so any other value is off either way - but an explicit "no" is greppable when
    /// somebody reports the opposite bug, and a deleted key is indistinguishable from a key that was
    /// never written, which is the state this whole fix exists to eliminate.
    ///
    /// Takes effect on the NEXT launch: debugFloatWanted() caches into a function-local static on
    /// first call, and GameClient::init calls it once. That matches how every other launcher option
    /// already behaves.
    /// </summary>
    public static bool WriteDebugMenu(bool on)
    {
        try
        {
            var p = OwnedPaths.OptionsIni;
            var dir = Path.GetDirectoryName(p);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var lines = File.Exists(p)
                ? File.ReadAllLines(p).Where(l => !IsKey(l, DebugMenuKey)).ToList()
                : new List<string>();

            while (lines.Count > 0 && lines[^1].Trim().Length == 0)
            {
                lines.RemoveAt(lines.Count - 1);
            }

            lines.Add($"{DebugMenuKey} = {(on ? "yes" : "no")}");
            IniLine.WriteAllLinesReplacing(p, lines);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Everything offered in the picker, smallest first: the common 4:3 and 16:9 sizes, whatever
    /// the primary screen reports, and <paramref name="current"/>.
    ///
    /// The last two are not padding. Without the screen's own size the list cannot express "native
    /// fullscreen" on anything unusual, and without the current value a game already set to a mode
    /// nobody listed would show an empty picker - which reads as "no resolution is set" and invites
    /// the user to change one they never meant to touch.
    /// </summary>
    public static List<Size> Modes(Size? current)
    {
        var all = new List<Size>
        {
            new(800, 600), new(1024, 768), new(1152, 864), new(1280, 960), new(1400, 1050), new(1600, 1200),
            new(1280, 720), new(1366, 768), new(1600, 900), new(1920, 1080), new(2560, 1440), new(3840, 2160),
        };

        // OFFER ONLY WHAT THE ADAPTER WILL ACTUALLY SET. The curated list above spans 800x600 to 4K
        // because it has to cover every machine; on any one machine most of it is unreachable, and a
        // picker that lists 3840x2160 on a 1440p monitor is offering a mode the game cannot enter.
        //
        // The supported set comes from EnumDisplaySettings rather than from Screen.Bounds, because
        // "smaller than the desktop" is not the same question as "supported": an adapter can refuse
        // an aspect ratio it has no scaler for, and it can offer modes LARGER than the current
        // desktop where the display does downscaling. Screen.Bounds cannot tell you either.
        var supported = SupportedModes();
        if (supported.Count > 0)
        {
            all = all.Where(supported.Contains).ToList();
        }
        else
        {
            // Enumeration failed - a remote session and a headless adapter both do this. Fall back to
            // the desktop size as a ceiling, which is wrong at the edges but never offers something
            // wildly unreachable.
            try
            {
                if (Screen.PrimaryScreen is { } s)
                {
                    all = all.Where(m => m.Width <= s.Bounds.Width && m.Height <= s.Bounds.Height).ToList();
                }
            }
            catch
            {
                // No display information at all: leave the curated list alone rather than empty it.
            }
        }

        // The mode the game is CURRENTLY set to is always offered, even when the adapter no longer
        // reports it - after a monitor swap it may not. Hiding it would leave the picker showing a
        // value the game is not using, or nothing at all, and would take away the only control that
        // lets somebody move off a resolution that no longer works.
        if (current is { Width: > 0, Height: > 0 } c)
        {
            all.Add(c);
        }

        return all.Distinct().OrderBy(m => m.Width).ThenBy(m => m.Height).ToList();
    }

    /// <summary>
    /// Every distinct width x height the primary adapter reports, ignoring refresh rate and colour
    /// depth - the same mode appears once per refresh rate and the game chooses neither.
    ///
    /// Empty means "could not tell", which the caller treats differently from "nothing is supported".
    /// </summary>
    private static HashSet<Size> SupportedModes()
    {
        var modes = new HashSet<Size>();
        try
        {
            var dm = new DEVMODE();
            dm.dmSize = (short)Marshal.SizeOf<DEVMODE>();
            for (var i = 0; EnumDisplaySettings(null, i, ref dm); i++)
            {
                if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0)
                {
                    modes.Add(new Size(dm.dmPelsWidth, dm.dmPelsHeight));
                }
            }
        }
        catch
        {
            // A P/Invoke that cannot resolve is a "could not tell", not a failure worth surfacing.
            return new HashSet<Size>();
        }
        return modes;
    }

    // null deviceName = the primary display device, which is the one the game opens on.
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplaySettings(string? deviceName, int modeNum, ref DEVMODE devMode);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DEVMODE
    {
        private const int CCHDEVICENAME = 32;
        private const int CCHFORMNAME   = 32;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHDEVICENAME)]
        public string dmDeviceName;
        public short dmSpecVersion;
        public short dmDriverVersion;
        public short dmSize;
        public short dmDriverExtra;
        public int   dmFields;
        public int   dmPositionX;
        public int   dmPositionY;
        public int   dmDisplayOrientation;
        public int   dmDisplayFixedOutput;
        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHFORMNAME)]
        public string dmFormName;
        public short dmLogPixels;
        public int   dmBitsPerPel;
        public int   dmPelsWidth;
        public int   dmPelsHeight;
        public int   dmDisplayFlags;
        public int   dmDisplayFrequency;
        public int   dmICMMethod;
        public int   dmICMIntent;
        public int   dmMediaType;
        public int   dmDitherType;
        public int   dmReserved1;
        public int   dmReserved2;
        public int   dmPanningWidth;
        public int   dmPanningHeight;
    }

    /// <summary>
    /// True when this line assigns <paramref name="key"/>. Matched case-insensitively so a
    /// hand-edited "resolution =" is replaced rather than left behind as a second line the engine
    /// ignores and a reader does not.
    ///
    /// GeneralsX @refactor Claude 13/08/2026 The body moved to IniLine, unchanged, when the HUD page
    /// became a second caller. Two copies of "what counts as the Resolution line" in two files is
    /// the exact shape of a pair that drifts silently - the same reasoning that emptied Apply.cs of
    /// its duplicate settings writer. This wrapper stays so the four call sites below are untouched.
    /// </summary>
    private static bool IsKey(string line, string key) => IniLine.IsKey(line, key);
}

/// <summary>
/// The launcher's window onto SagePatch.ini - the game's per-device camera / client-tuning file.
///
/// UNLIKE Options.ini this file is OURS. The engine loads it as a GameData override but restricts the
/// load to six client-side fields (GlobalData::s_SagePatchFieldParseTable - Max/Min camera height,
/// EnforceMaxCameraHeight, KeyboardScrollSpeedFactor, TerrainDrawDistanceScale,
/// ShellMapSoundVolumeScale), so nothing written
/// here can reach the shared simulation: a launcher cannot desync a lockstep match through it. The
/// engine writes a default copy only when the file is ABSENT; once the launcher writes it, the
/// launcher owns the camera block.
///
/// Full-block ownership rather than the read-drop-append dance GameOptions does for Options.ini,
/// precisely because this file is ours and small: writing the whole GameData block keeps the QoL
/// baseline (a higher, UNIFORM maximum zoom and softer terrain pop-in) even when the user opens this
/// screen before the game has ever created the file. MaxCameraHeight is deliberately fixed and never
/// exposed - a per-player maximum zoom is a see-further advantage and must stay lobby-wide, so the
/// only camera fields the launcher lets a person set are MinCameraHeight and the keyboard scroll speed.
/// </summary>
internal static class GameSagePatch
{
    // THE MAXIMUM IS GENERALS ONLINE'S, THE MINIMUM IS THE ENGINE'S. Their launcher ships Maximum
    // 310, Minimum 210, Movement Speed 100%, and this file matched all three until 22/09/2026 on the
    // reasoning that it is the camera feel this game's players already have in their hands.
    //
    // THE OWNER CHANGED THE MINIMUM TO 100 on 22/09/2026. 210 stops you zooming in as far as the
    // engine itself allows, and the debug build showing 100 while the launcher wrote 210 was the
    // visible symptom. The engine's own vanilla minimum is 100 and the SagePatch file it writes when
    // absent has min 100 (max 350 until 25/09/2026, 310 since), so 100 is the engine's number rather
    // than a new invention. The maximum stays 310 and is still not a control - see FixedMaxHeight above.
    //
    // gxio's MIN_CAMERA_HEIGHT_DEFAULT (sagepatch.rs) was changed in the same commit and must stay
    // equal to this: two launchers that disagree about the default camera height are two different
    // games.
    //
    // KeyboardScrollSpeedFactor 1.0 == 100%.
    public const int DefaultMinHeight = 100;
    public const int DefaultMovePct   = 100;

    // Fixed, uniform, and never a control: a per-player maximum zoom is a see-further advantage. 310
    // is Generals Online's value, where it is host-authoritative and lobby-wide. Until that is
    // propagated through the lobby here, everyone gets the same number rather than nobody getting a
    // choice they could abuse.
    private const int FixedMaxHeight  = 310;

    private const string MinKey   = "MinCameraHeight";
    private const string SpeedKey = "KeyboardScrollSpeedFactor";

    public readonly record struct Camera(int MinHeight, int MovePct);

    /// <summary>
    /// Write the defaults ONCE, if nobody has written SagePatch.ini yet.
    ///
    /// Without this, "the defaults match Generals Online" would only be true for people who opened
    /// this screen and pressed SAVE. The ENGINE writes its own SagePatch.ini the first time it starts
    /// without one (GameEngine.cpp), using its vanilla-ish numbers - so whoever writes first wins, and
    /// if the launcher never does, the engine's pair becomes the default after all (100/350 until
    /// 25/09/2026; the engine now writes this launcher's 100/310).
    ///
    /// Only ever writes when the file is ABSENT, so it can never overwrite a value somebody chose.
    /// </summary>
    public static void EnsureDefaults()
    {
        try
        {
            // The folder the engine reads NEXT, not the launcher's pre-migration one (review of
            // 2927d052a, finding 1) - see OwnedPaths.SagePatchIniForDefaults.
            var p = OwnedPaths.SagePatchIniForDefaults;
            if (!File.Exists(p))
            {
                WriteCamera(DefaultMinHeight, DefaultMovePct, p);
            }
        }
        catch
        {
            // Nothing here is worth blocking the launcher from opening.
        }
    }

    /// <summary>The camera values currently in SagePatch.ini, or the defaults if absent.</summary>
    public static Camera ReadCamera()
    {
        var min = DefaultMinHeight;
        var pct = DefaultMovePct;
        try
        {
            var p = OwnedPaths.SagePatchIni;
            if (File.Exists(p))
            {
                foreach (var line in File.ReadAllLines(p))
                {
                    if (TryValue(line, MinKey, out var mv))
                    {
                        min = (int)Math.Round(mv);
                    }
                    else if (TryValue(line, SpeedKey, out var sv))
                    {
                        pct = (int)Math.Round(sv * 100.0);
                    }
                }
            }
        }
        catch
        {
            // An unreadable SagePatch.ini means we fall back to the defaults, same as a fresh install.
        }
        return new Camera(Math.Clamp(min, 50, 300), Math.Clamp(pct, 50, 300));
    }

    /// <summary>Write the whole GameData block with the user's two values and the fixed QoL baseline.</summary>
    public static bool WriteCamera(int minHeight, int movePct, string? path = null)
    {
        try
        {
            var p = path ?? OwnedPaths.SagePatchIni;
            var dir = Path.GetDirectoryName(p);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }

            var min = Math.Clamp(minHeight, 50, 300);
            // InvariantCulture, or a German locale writes "1,5" and the engine's parseReal reads 1.
            var speed = (Math.Clamp(movePct, 50, 300) / 100.0).ToString("0.0#", CultureInfo.InvariantCulture);

            var b = new System.Text.StringBuilder();
            b.AppendLine("; GeneralsX camera / client tuning, written by the launcher. Safe to delete.");
            b.AppendLine("; The engine restricts this file to six client-side fields, so nothing here");
            b.AppendLine("; can affect the shared simulation or desync a match.");
            b.AppendLine("GameData");
            b.AppendLine($"  MaxCameraHeight = {FixedMaxHeight}.0");
            b.AppendLine($"  MinCameraHeight = {min}.0");
            b.AppendLine("  EnforceMaxCameraHeight = No");
            b.AppendLine($"  KeyboardScrollSpeedFactor = {speed}");
            b.AppendLine("  TerrainDrawDistanceScale = 1.05");
            // GeneralsX @feature Claude 10/09/2026 Must match the RUST launcher's render() - this file's
            // text is the launcher's own shape, NOT the engine's generated block, which has different
            // comments and camera values. Only the key and its default are shared with the engine. This
            // method replaces the WHOLE file, so omitting the key here would delete it every time a
            // player saves from the Graphics screen.
            b.AppendLine("  ShellMapSoundVolumeScale = 50%");
            b.AppendLine("End");
            File.WriteAllText(p, b.ToString(), new System.Text.UTF8Encoding(false));
            return true;
        }
        catch
        {
            // A read-only Documents folder must not take the launcher down on the way to a game.
            return false;
        }
    }

    /// <summary>Parse the engine's own "  Key = 1.5" line (leading space, invariant decimal point).</summary>
    private static bool TryValue(string line, string key, out double value)
    {
        value = 0;
        var t = line.TrimStart();
        if (!t.StartsWith(key, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        var rest = t[key.Length..].TrimStart();
        if (!rest.StartsWith('='))
        {
            return false;
        }
        return double.TryParse(rest[1..].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out value);
    }
}
