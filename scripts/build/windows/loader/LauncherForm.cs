/* The launcher: the thing a friend runs every time they want to play.
 *
 * GeneralsX @feature Claude 09/08/2026
 *
 * ONE SCREEN. Logo, a line of live status, and three buttons: Exit, Options, Play. That is the
 * whole surface. It is modelled directly on the Generals Online launcher because that is the shape
 * players of this game already know, and because the alternative - a wizard that asks questions -
 * is what we are deleting. Nobody types a server address here, or anywhere, ever.
 *
 * WHY THE CONTROL BOX IS HAND-DRAWN. MetroForm paints its own non-client area and places its
 * caption buttons from the width it had when they were created. This form is resized after
 * construction, so the library's close button ended up at the TOP LEFT while the minimise glyph
 * stayed at the right - visible in every screenshot of the old build. Rather than fight the
 * library's non-client painting, ControlBox is off and the two glyphs are ordinary controls anchored
 * to the top-right corner. It also happens to be exactly where Generals Online puts them.
 *
 * WHAT RUNS BEFORE PLAY. Play is not a shortcut to the game. It runs the updater first, every time,
 * and only launches once the install matches the signed manifest. That is the whole point of having
 * a launcher rather than a desktop shortcut to generalszh.exe.
 */

using System.Diagnostics;
using System.Runtime.InteropServices;   // ReleaseCapture / SendMessage for the window drag

namespace GeneralsX.Loader;

internal sealed class LauncherForm : Form
{
    private readonly Label _title = new()
    {
        Text = "GENERALS", AutoSize = false, Dock = DockStyle.None,
        TextAlign = ContentAlignment.MiddleCenter, UseMnemonic = false,
        ForeColor = Color.White, Font = new Font("Segoe UI", 44f, FontStyle.Bold),
    };

    private readonly Label _titleX = new()
    {
        Text = "X", AutoSize = false, TextAlign = ContentAlignment.MiddleCenter, UseMnemonic = false,
        ForeColor = LauncherTheme.Accent, Font = new Font("Segoe UI", 44f, FontStyle.Bold),
    };

    private readonly Label _sub = new()
    {
        Text = "Zero Hour on Windows, macOS, Linux and iPadOS", AutoSize = false,
        TextAlign = ContentAlignment.MiddleCenter, UseMnemonic = false,
        ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 10f),
    };

    /// <summary>
    /// The live line under the logo. Generals Online shows players and lobbies here; we show the
    /// same, plus the build we are on, because "am I on the same version as my friends" is the
    /// question this project's whole join path turns on.
    /// </summary>
    private readonly Label _status = new()
    {
        Text = "Checking…", AutoSize = false, TextAlign = ContentAlignment.MiddleCenter,
        UseMnemonic = false, ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9.5f),
    };

    private readonly BigButton _exit = new("EXIT", LauncherTheme.Bg, LauncherTheme.Border) { Ghost = true };
    private readonly BigButton _options = new("OPTIONS", LauncherTheme.Bg, LauncherTheme.Border) { Ghost = true };

    /// <summary>
    /// Blue normally, YELLOW when the check found an update - the owner's ask, and the colour is
    /// driven by the same plan that writes the "Update" line so the two cannot disagree. Built blue
    /// because that is the state before any answer has arrived: an unanswered check must not
    /// announce an update it has not found.
    /// </summary>
    private readonly BigButton _changelogBtn =
        new("VIEW CHANGELOGS", LauncherTheme.PlayFace, LauncherTheme.PlayBorder);

    private readonly BigButton _play = new("PLAY", LauncherTheme.PlayFace, LauncherTheme.PlayBorder);

    // GeneralsX @bugfix Claude 11/08/2026 There is no LOG IN button here on purpose. The one that
    // used to sit left of EXIT drove an RFC 8628 device flow through GeneralsXUpdate.exe --login,
    // POSTing to /device and /claim - endpoints tools/oauth/worker.js deliberately does not serve
    // and has tests asserting 404. Every press failed on its first request, so this was a control
    // that could not work on any machine. Signing in is the ENGINE's own loopback flow; the
    // launcher only reports login state, from the assertion file the engine writes.

    private readonly Label _close = new()
    {
        Text = "✕", AutoSize = false, Size = new Size(40, 28), UseMnemonic = false,
        TextAlign = ContentAlignment.MiddleCenter, ForeColor = LauncherTheme.TextDim,
        Font = new Font("Segoe UI", 11f), Cursor = Cursors.Hand,
        Anchor = AnchorStyles.Top | AnchorStyles.Right,
    };

    private readonly Label _min = new()
    {
        Text = "—", AutoSize = false, Size = new Size(40, 28), UseMnemonic = false,
        TextAlign = ContentAlignment.MiddleCenter, ForeColor = LauncherTheme.TextDim,
        Font = new Font("Segoe UI", 9f), Cursor = Cursors.Hand,
        Anchor = AnchorStyles.Top | AnchorStyles.Right,
    };

    /// <summary>Progress strip shown only while Play is doing the update.</summary>
    private readonly ProgressBar _bar = new()
    {
        Dock = DockStyle.Bottom, Height = 4, Style = ProgressBarStyle.Continuous,
        Minimum = 0, Maximum = 1000, Visible = false,
    };

    private readonly Label _work = new()
    {
        Dock = DockStyle.Bottom, Height = 22, AutoSize = false, Visible = false, UseMnemonic = false,
        TextAlign = ContentAlignment.MiddleCenter, ForeColor = LauncherTheme.TextDim,
        Font = new Font("Segoe UI", 9f),
    };

    public LauncherForm()
    {
        Text = "GeneralsX Launcher";

        // GeneralsX @refactor Claude 19/08/2026 W5. MetroModernUI is gone, and this form is a plain
        // Form again. It was contributing three things to the window, none of them wanted and none
        // of them written here:
        //
        //   * MetroForm.OnPaint cleared the whole client area to #111111 on every frame, painting
        //     over the BackColor set below. Every non-transparent label therefore sat on a faintly
        //     lighter block, and LauncherTheme.Bg described the ROWS rather than the ground.
        //   * a 5px #00AEDB strip across the full width of the top - its border brush, a second and
        //     more saturated blue than the #3B8EEA accent this launcher actually uses.
        //   * the window Text drawn a second time as a 24px header at (20,20), above the wordmark.
        //     ControlBox = false removed its caption buttons; it never removed this.
        //
        // What made the dependency unkeepable was none of those - it was MetroCheckBox, which
        // hardcodes a 12px tick box and builds its label font in PIXEL units inside OnPaint, where
        // no DPI mechanism can reach it. See GxCheckBox.
        //
        // Almost nothing is lost: this form already drew its own chrome, hit-tested its own borders
        // in WndProc, and ran with ControlBox off and FormBorderStyle.None.
        ControlBox = false;
        MaximizeBox = true;
        MinimizeBox = false;
        FormBorderStyle = FormBorderStyle.None;

        // MetroForm's DefaultPadding was (20,60,20,20) and a plain Form has none, so every Dock=Fill
        // child would gain 100px of width and 80px of height the moment the base class changed.
        // Carried over explicitly to keep W5's visual delta to the three artefacts above; whether
        // that 60px top inset is still the right number is a W7 question, not this one.
        Padding = new Padding(20, 60, 20, 20);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = LauncherTheme.Bg;
        ForeColor = LauncherTheme.Text;

        // GeneralsX @feature Claude 16/08/2026 THE WINDOW RESIZES NOW.
        //
        // It used to be pinned with MinimumSize = MaximumSize = the start size, which is the one
        // arrangement Windows cannot resize by any route: no drag, no maximise, no Win+Arrow, no
        // snap. 900x480 stays the FLOOR because the options rows have a real minimum legible width,
        // but there is no ceiling - the preview and the option rows both earn every extra pixel.
        //
        // MaximizeBox matters even with ControlBox off and no maximise glyph of our own: it is what
        // lets Aero Snap and Win+Up work, and those are how most people actually maximise.
        ClientSize = new Size(900, 480);
        MinimumSize = new Size(900, 480);
        MaximumSize = Size.Empty;

        try
        {
            var exe = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(exe) && File.Exists(exe))
            {
                Icon = Icon.ExtractAssociatedIcon(exe);
            }
        }
        catch
        {
            // A missing icon is not worth failing a launch over.
        }

        BuildLayout();
        Wire();
    }

    /// <summary>
    /// The layout is deliberately NOT the reference launcher's centred logo over three identical
    /// buttons. Three equal buttons give Exit the same visual weight as Play, which is wrong: one
    /// of them is what everybody came here to press. So the wordmark sits left on a baseline, the
    /// state of the install is a readable block rather than a single grey sentence, and Play is a
    /// filled primary on the right with the other two as quiet ghost buttons beside it.
    /// </summary>
    private void BuildLayout()
    {
        Controls.Add(_bar);
        Controls.Add(_work);

        // Placed once, at the starting size; Anchor = Top|Right keeps them in the corner from here on.
        _close.Location = new Point(ClientSize.Width - 46, 8);
        _min.Location = new Point(ClientSize.Width - 88, 8);
        Controls.Add(_close);
        Controls.Add(_min);

        _title.TextAlign = ContentAlignment.MiddleLeft;
        _titleX.TextAlign = ContentAlignment.MiddleLeft;
        Controls.Add(_title);
        Controls.Add(_titleX);

        Controls.Add(_sub);
        Controls.Add(_rule);

        _status.TextAlign = ContentAlignment.TopLeft;
        _status.Font = new Font("Segoe UI", 10f);
        Controls.Add(_status);

        // Play dominates; Options and Exit are quiet. Right-aligned so the primary sits under the
        // thumb's natural resting place rather than in the far corner.
        //
        // VIEW CHANGELOGS sits LEFT OF THE GHOSTS, not beside Play, and is narrower than Play even
        // though both are filled. It carries a border colour, so putting it next to Play would give
        // the row two competing primaries; separated by the two quiet buttons, the eye still lands
        // on Play first. Its extra width is the label - "VIEW CHANGELOGS" does not fit a ghost.
        Controls.Add(_exit);
        Controls.Add(_options);
        Controls.Add(_changelogBtn);
        Controls.Add(_play);

        PositionChrome();
    }

    /// <summary>
    /// Every coordinate on the home view, recomputed from the CURRENT ClientSize.
    ///
    /// GeneralsX @feature Claude 16/08/2026 Split out of BuildLayout so it can run again. BuildLayout
    /// both created controls and positioned them, so it could only ever run once - which was fine
    /// while the window was pinned to one size and is the whole bug the moment it is not.
    ///
    /// Two things are deliberately anchored to the BOTTOM edge rather than measured from the top:
    /// the button row and the status block's height. With a top-anchored button row at y=372, a
    /// taller window left Play stranded in the middle of a growing empty space, which reads as a
    /// layout failure rather than a bigger window. Everything else keeps its top-left origin,
    /// because the wordmark belongs to the top-left corner and should not drift.
    /// </summary>
    private void PositionChrome()
    {
        const int left = 44, logoTop = 78, edge = 44;
        const int bh = 56, gap = 10;

        var w = ClientSize.Width;
        var h = ClientSize.Height;

        // NOT the caption glyphs. _close and _min carry Anchor = Top|Right (see their declarations),
        // and WinForms anchoring re-derives a control's position on every resize from the offset it
        // recorded when the anchor took effect. Assigning Location here as well made the two fight:
        // the ✕ drifted into the middle of the title bar while the — stayed in the corner. Anchor
        // alone is correct and is set once in BuildLayout.

        // GeneralsX @bugfix Claude 19/08/2026 W2. These two were rebuilt on every PositionChrome
        // call, which is every resize event, and the previous Font was dropped rather than
        // disposed. Two GDI handles per resize tick. The wordmark never changes size, so it is
        // built once and assigned once; when W6 makes geometry DPI-aware this becomes the single
        // place the wordmark's size is decided, rather than a per-resize allocation to unpick.
        _wordmarkFont ??= new Font("Segoe UI", 40f, FontStyle.Bold);
        if (!ReferenceEquals(_title.Font, _wordmarkFont))
        {
            _title.Font = _wordmarkFont;
            _titleX.Font = _wordmarkFont;
        }
        _title.Bounds = new Rectangle(left, logoTop, 330, 62);
        _titleX.Bounds = new Rectangle(left + 316, logoTop, 70, 62);

        _sub.Bounds = new Rectangle(left + 4, logoTop + 62, Math.Max(200, w - left - edge), 22);
        _rule.Bounds = new Rectangle(left, logoTop + 92, 96, 3);

        // The button row sits a fixed distance off the BOTTOM; the status block then takes whatever
        // vertical space is left between the accent rule and that row, so it grows with the window
        // instead of keeping a fixed 96px and leaving a hole underneath.
        var by = h - edge - bh;
        var statusTop = logoTop + 116;
        _status.Bounds = new Rectangle(
            left, statusTop, Math.Max(200, w - left - edge), Math.Max(48, by - statusTop - 16));

        const int playW = 220, ghostW = 132, logW = 178;
        var right = w - edge;
        _play.Bounds = new Rectangle(right - playW, by, playW, bh);
        _options.Bounds = new Rectangle(right - playW - gap - ghostW, by, ghostW, bh);
        _exit.Bounds = new Rectangle(right - playW - gap - ghostW - gap - ghostW, by, ghostW, bh);
        _changelogBtn.Bounds =
            new Rectangle(right - playW - gap - ghostW - gap - ghostW - gap - logW, by, logW, bh);
    }

    protected override void OnClientSizeChanged(EventArgs e)
    {
        base.OnClientSizeChanged(e);
        // Guard the pre-construction call: OnClientSizeChanged fires while the base constructor sets
        // ClientSize, before BuildLayout has added anything.
        if (_play.IsHandleCreated || Controls.Contains(_play))
        {
            PositionChrome();
        }
    }

    /// <summary>A short accent rule under the wordmark. Cheaper than a logo bitmap and it scales.</summary>
    private readonly Panel _rule = new() { BackColor = LauncherTheme.Accent };

    private void Wire()
    {
        _close.Click += (_, _) => Close();
        _close.MouseEnter += (_, _) => _close.ForeColor = Color.White;
        _close.MouseLeave += (_, _) => _close.ForeColor = LauncherTheme.TextDim;
        _min.Click += (_, _) => WindowState = FormWindowState.Minimized;
        _min.MouseEnter += (_, _) => _min.ForeColor = Color.White;
        _min.MouseLeave += (_, _) => _min.ForeColor = LauncherTheme.TextDim;

        _exit.Click += (_, _) => Close();
        _options.Click += (_, _) => ShowOptions(true);
        _changelogBtn.Click += (_, _) => ShowChangelog(true);
        _play.Click += async (_, _) => await PlayAsync();

        // A borderless window still has to be draggable, or it is nailed to the centre of the
        // screen. Dragging by the background is what the reference launcher does too.
        foreach (Control c in new Control[] { this, _title, _titleX, _sub, _status })
        {
            c.MouseDown += DragFrom;
        }

        Shown += async (_, _) => await RefreshStatusAsync();
    }

    private const int WM_NCLBUTTONDOWN = 0xA1;
    private const int HTCAPTION = 0x2;

    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    /// <summary>
    /// Drag a borderless window by handing the press to the OS instead of tracking it by hand. The
    /// control that raised this - one of the labels, or the form background - took mouse capture on
    /// its WM_LBUTTONDOWN. The old code then subscribed the FORM's MouseMove/MouseUp, but while a
    /// child label holds capture the form's MouseUp never fires, so _dragging stayed true and the
    /// window followed the cursor with no button held. ReleaseCapture drops that capture and
    /// WM_NCLBUTTONDOWN/HTCAPTION hands off to DefWindowProc's own modal move loop, which is DPI-,
    /// multi-monitor- and snap-correct and ends the instant the button is released - so nothing can
    /// be left tracking the mouse.
    /// </summary>
    private void DragFrom(object? sender, MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Left)
        {
            return;
        }
        ReleaseCapture();
        SendMessage(Handle, WM_NCLBUTTONDOWN, (IntPtr)HTCAPTION, IntPtr.Zero);
    }

    // GeneralsX @feature Claude 16/08/2026 RESIZE BORDERS FOR A BORDERLESS WINDOW.
    //
    // FormBorderStyle.None means Windows draws no frame, and a window with no frame has nothing to
    // grab: every edge reports HTCLIENT, so the cursor never becomes a resize arrow and a drag on
    // the edge does nothing. Setting MinimumSize/MaximumSize apart is necessary but not sufficient.
    //
    // The fix is to answer WM_NCHITTEST ourselves. Reporting HTLEFT/HTBOTTOMRIGHT/etc. for a band
    // along each edge hands the drag to DefWindowProc's own sizing loop - the same one a normal
    // window uses - so we inherit correct cursors, snapping, DPI behaviour and the Escape-cancels
    // convention for free, rather than tracking mouse deltas by hand.
    //
    // base.WndProc runs FIRST and is only overridden where it said HTCLIENT. That keeps MetroForm's
    // own hit-testing authoritative for everything it claims, so a child control or a caption glyph
    // still wins over the border band.
    private const int WM_NCHITTEST = 0x84;
    private const int HTCLIENT = 1;
    private const int HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13;
    private const int HTTOPRIGHT = 14, HTBOTTOM = 15, HTBOTTOMLEFT = 16, HTBOTTOMRIGHT = 17;

    /// <summary>Grab band, in pixels. Wide enough to hit without aiming, narrow enough that the
    /// caption glyphs in the corner are still clickable.</summary>
    private const int ResizeBand = 6;

    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);

        if (m.Msg != WM_NCHITTEST || m.Result.ToInt32() != HTCLIENT)
        {
            return;
        }
        // A maximised window has no edges to drag.
        if (WindowState != FormWindowState.Normal)
        {
            return;
        }

        var screen = new Point(unchecked((short)(long)m.LParam), unchecked((short)((long)m.LParam >> 16)));
        var p = PointToClient(screen);

        var onLeft = p.X <= ResizeBand;
        var onRight = p.X >= ClientSize.Width - ResizeBand;
        var onTop = p.Y <= ResizeBand;
        var onBottom = p.Y >= ClientSize.Height - ResizeBand;

        int hit;
        if (onTop && onLeft) { hit = HTTOPLEFT; }
        else if (onTop && onRight) { hit = HTTOPRIGHT; }
        else if (onBottom && onLeft) { hit = HTBOTTOMLEFT; }
        else if (onBottom && onRight) { hit = HTBOTTOMRIGHT; }
        else if (onLeft) { hit = HTLEFT; }
        else if (onRight) { hit = HTRIGHT; }
        else if (onTop) { hit = HTTOP; }
        else if (onBottom) { hit = HTBOTTOM; }
        else { return; }

        m.Result = (IntPtr)hit;
    }

    /// <summary>
    /// The 40pt bold behind the GENERALS X wordmark, built once. See PositionChrome.
    /// </summary>
    private Font? _wordmarkFont;

    private OptionsPanel? _optionsView;

    /// <summary>
    /// Swap between the home view and options INSIDE this window. Nothing new is created on screen,
    /// nothing new appears on the taskbar, and the frame - title, close, minimise - never moves.
    /// </summary>
    public void ShowOptions(bool show)
    {
        if (show)
        {
            _optionsView ??= new OptionsPanel(() => ShowOptions(false), ClientSize);
            if (!Controls.Contains(_optionsView))
            {
                Controls.Add(_optionsView);
            }
            _optionsView.Visible = true;
            _optionsView.BringToFront();
        }
        else if (_optionsView is not null)
        {
            _optionsView.Visible = false;
        }

        HideHomeView(show);
    }

    private ChangelogPanel? _changelogView;

    /// <summary>
    /// The changelog, in this window, as a view - the same swap Options does. Not a browser: the
    /// notes are two paragraphs and sending somebody out of the launcher to read them is a worse
    /// experience than the button being useless.
    ///
    /// REBUILT ON EVERY SHOW rather than cached with `??=` as Options is, because its content
    /// arrives from the network after the form is constructed. A cached panel would show whatever
    /// the changelog was the first time it was opened.
    /// </summary>
    /// <summary>Open the settings view on a given page. See OptionsPanel.SelectPage.</summary>
    public void ShowOptionsPage(int page)
    {
        ShowOptions(true);
        _optionsView?.SelectPage(page);
    }

    /// <summary>
    /// Photograph every view into <paramref name="dir"/> and close.
    ///
    /// GeneralsX @feature Claude 19/08/2026 W0 of the layout restructure, and the step the rest of
    /// it rests on: with no UI tests, "the rebuild looks identical" is only checkable if the same
    /// arguments produce the same pixels twice. Files are named {dpi}-{w}x{h}-{view}.png so a whole
    /// size x DPI matrix lands in one directory without collisions.
    ///
    /// DrawToBitmap rather than a screen grab, deliberately: it renders the form's own controls and
    /// is unaffected by what is in front of the window, by the desktop wallpaper, or by whether
    /// anybody is logged in at the console. A screen grab would need the session-1 interactive task
    /// and would photograph whatever happened to overlap.
    ///
    /// DoEvents twice per view because the page is built on Select() and then re-laid by
    /// LayoutRows() on the SizeChanged that follows; one pump catches the build and the second the
    /// reflow. Without the second, rows measured by IReflowsToWidth are photographed at their
    /// pre-reflow height.
    /// </summary>
    public void SnapAllViews(string dir)
    {
        try
        {
            Directory.CreateDirectory(dir);
            var dpi = DeviceDpi;
            var cs = ClientSize;

            void Shot(string name)
            {
                Application.DoEvents();
                Application.DoEvents();
                using var bmp = new Bitmap(Size.Width, Size.Height);
                DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
                bmp.Save(Path.Combine(dir, $"{dpi}-{cs.Width}x{cs.Height}-{name}.png"),
                    System.Drawing.Imaging.ImageFormat.Png);
            }

            ShowOptions(false);
            ShowChangelog(false);
            Shot("home");

            // GeneralsX @bugfix wh1ter0se 20/08/2026 Was a hardcoded 6. Adding a seventh category
            // would have left it unphotographed, and a page nobody looks at is a page nobody checks.
            for (var i = 0; i < OptionsPanel.CategoryCount; i++)
            {
                ShowOptionsPage(i);
                Shot($"options{i}");
                // A second frame at the bottom of the scroll. Costs one PNG and is the only way a
                // page whose content exceeds the window gets its lower half reviewed at all.
                _optionsView?.ScrollContent(true);
                Shot($"options{i}-bottom");
                _optionsView?.ScrollContent(false);
            }
            ShowOptions(false);

            ShowChangelog(true);
            Shot("changelog");
            ShowChangelog(false);
        }
        catch (Exception ex)
        {
            // A harness that dies silently is worse than one that fails loudly: the caller compares
            // file counts and would read "no files" as "no differences".
            Console.Error.WriteLine($"--snap failed: {ex}");
        }
        finally
        {
            Close();
        }
    }

    public void ShowChangelog(bool show)
    {
        if (show)
        {
            if (_changelogView is not null)
            {
                Controls.Remove(_changelogView);
                _changelogView.Dispose();
            }
            _changelogView = new ChangelogPanel(_changelog, () => ShowChangelog(false), ClientSize);
            Controls.Add(_changelogView);
            _changelogView.Visible = true;
            _changelogView.BringToFront();
        }
        else if (_changelogView is not null)
        {
            _changelogView.Visible = false;
        }

        HideHomeView(show);
    }

    /// <summary>
    /// Show or hide everything that belongs to the home view. Listing the controls explicitly beats
    /// a "hide all except" loop, which silently swallows any control added later - and one WAS added
    /// later: the changelog button, which without this line stayed painted on top of Options.
    /// </summary>
    private void HideHomeView(bool hidden)
    {
        foreach (var c in new Control[]
                 { _title, _titleX, _sub, _rule, _status, _exit, _options, _changelogBtn, _play })
        {
            c.Visible = !hidden;
        }
        _close.BringToFront();
        _min.BringToFront();
    }

    private void SetWork(string text, bool visible)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => SetWork(text, visible));
            return;
        }
        _work.Text = text;
        _work.Visible = visible;
        _bar.Visible = visible;
    }

    /// <summary>
    /// What the status line says. Deliberately never shows a server ADDRESS - this pane is what
    /// people screenshot when they ask for help.
    /// </summary>
    private async Task RefreshStatusAsync()
    {
        var dir = OwnedPaths.InstallDir;
        var installed = Apply.GameInstalled(dir);

        // Read the build the updater recorded rather than anything compiled in here, so the number
        // on screen is the number that will be compared at join time.
        //
        // GeneralsX @feature Claude 10/08/2026 Owner's ask: show WHICH build and WHEN, not a raw
        // ordinal. Both are already in gxinstalled.txt: the ordinal IS the engine commit's unix
        // time, and the "item" line names the applied artifact - whose filename carries the
        // engine commit, as "GeneralsX-win64-<commit>.zip". Nothing new is recorded; this only
        // renders what the updater always wrote.
        //
        // THAT NAME ENDS AT THE COMMIT, and this comment used to say "<commit>-<contenthash>.zip",
        // which is a shape that must never ship: state_load (gxlaunch.c:1609-1620) takes the text
        // after the LAST '-' and compares its first 7 characters to BUILD.txt's commit, discarding
        // the entire install record when nothing agrees. A content hash after the commit would make
        // that guard fire on every launch and re-download the whole payload every time. Since
        // 12/08/2026 the payload is split into four artifacts and the other three ARE named by
        // content - deliberately with no '-' at all, so strrchr() ignores them and this one stays
        // the only artifact the guard reads. See package-windows-share.ps1's "THE SPLIT".
        var ordinal = "unknown";
        var version = "";
        var buildDate = "";
        try
        {
            // gxinstalled.txt opens with a '#' banner explaining what it is, so the FIRST line is
            // not the ordinal - reading it blindly put "# GeneralsX installed state. Written by the
            // launcher; delete to force a re-download." on screen under the heading "Build".
            var stateFile = Path.Combine(dir, "gxinstalled.txt");
            if (File.Exists(stateFile))
            {
                foreach (var raw in File.ReadLines(stateFile))
                {
                    var l = raw.Trim();
                    if (l.Length == 0 || l.StartsWith('#') || l.StartsWith(';'))
                        continue;
                    if (l.StartsWith("item ", StringComparison.OrdinalIgnoreCase))
                    {
                        // GeneralsX @bugfix wh1ter0se 12/08/2026 The trailing '-' required a name
                        // shaped "GeneralsX-win64-<commit>-<contenthash>.zip". Nothing has ever
                        // published that shape - every payload is "GeneralsX-win64-<commit>.zip" -
                        // so this match could never succeed, version stayed empty, and the label
                        // fell through to the ordinal. Accept the separator OR the extension.
                        var m = System.Text.RegularExpressions.Regex.Match(l, @"-win64-([0-9a-f]{7,40})(?:-|\.)");
                        if (m.Success && version.Length == 0) version = m.Groups[1].Value;
                        continue;
                    }
                    if (ordinal == "unknown")
                    {
                        // The line may be "ordinal <n>" or a bare number depending on who wrote it.
                        var parts = l.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                        ordinal = parts.FirstOrDefault(p => p.All(char.IsDigit)) ?? l;
                    }
                }
                if (long.TryParse(ordinal, out var unix) && unix > 0)
                    buildDate = DateTimeOffset.FromUnixTimeSeconds(unix).ToLocalTime().ToString("yyyy-MM-dd HH:mm");
            }
        }
        catch
        {
            // An unreadable state file is not worth blocking the launcher over.
        }

        // GeneralsX @feature wh1ter0se 12/08/2026 A fresh install's record carries "ordinal 0" -
        // the installer writes what it knows and it cannot know the manifest ordinal - so the date
        // has to come from somewhere else until the launcher applies an update and rewrites the
        // record. BUILD.txt ships inside the payload and states when that payload was packaged,
        // which is exactly the question, and it is already on disk.
        if (buildDate.Length == 0)
            buildDate = ReadPackagedDate(dir);

        var build = version.Length > 0 ? version : ordinal;
        if (buildDate.Length > 0)
            build += $"   ({buildDate})";

        // FIRST FRAME: the cache, and it is labelled as the cache. No HTTP on the path that paints
        // the window - a launcher that shows nothing until a network round-trip finishes is a
        // launcher that looks broken on a bad connection. What this cannot do is say "up to date",
        // because gxmanifest.txt is the manifest as of the PREVIOUS run and therefore agrees with
        // the install by construction; see UpdateStatus.CacheOnlyVerdict.
        var (availBuild, cacheSummary) = ReadCacheVerdict(dir, version);

        // GeneralsX @feature Claude 19/08/2026 A fixed status block under --snap. This is the one
        // line on the home view that varies with the machine - install path, build number, what the
        // cached manifest last said - so two runs a second apart would differ here and nowhere
        // else, and a byte comparison that is never zero is a comparison nobody trusts.
        //
        // Five lines, because the real block is five lines and the layout must not move. Content
        // only; anything that changed a HEIGHT here would defeat the harness it exists to serve.
        if (Program.SnapMode)
        {
            _status.Text =
                "Installed        C:\\snap\\GeneralsX\n" +
                "Build            0000000000  0000-00-00\n" +
                "Available        0000000000  0000-00-00\n" +
                "Update           up to date\n" +
                "On Play          updates first, then starts the game";
            _play.Text = "PLAY";
            return;
        }

        _status.Text = installed
            ? $"Installed        {dir}\n" +
              $"Build            {build}\n" +
              $"Available        {availBuild}\n" +
              $"Update           {cacheSummary}\n" +
              $"On Play          updates first, then starts the game"
            : "Not installed yet\n" +
              "Press Play and it will download and set everything up.\n" +
              "Your Zero Hour installation is only ever read from.";

        _play.Text = installed ? "PLAY" : "INSTALL";

        // SECOND FRAME: ask the updater what Play would actually do. This is the whole fix - the
        // panel now runs the same computation the click runs, before the click, and says how many
        // megabytes it is. Fire-and-forget on purpose: it must never gate the window, and every
        // failure inside it lands on "could not check".
        if (installed)
        {
            _ = RefreshPlanAsync(dir);
        }
        await Task.CompletedTask;
    }

    private UpdatePlan _plan = UpdatePlan.Unknown;
    private Changelog _changelog = Changelog.Empty;

    /// <summary>
    /// Run the real check off the UI thread and repaint the verdict when it answers.
    ///
    /// GeneralsX @bugfix Claude 13/08/2026 This is the call whose absence WAS the bug. Everything
    /// the panel used to show came from files the previous run left behind, so "Available" could
    /// only ever echo the install and "Missing none" only meant "no artifact is absent" - never
    /// "nothing will download". The owner read "up to date" and then watched 60 MB arrive.
    /// </summary>
    private async Task RefreshPlanAsync(string dir)
    {
        // The changelog and the check are independent and both are slow; running them together
        // means the button colour and the notes settle at the same time rather than in two steps.
        var checkTask = Apply.CheckAsync(dir);
        var logTask = ChangelogSource.LoadAsync(dir);
        var plan = await checkTask.ConfigureAwait(false);
        var log = await logTask.ConfigureAwait(false);

        if (IsDisposed || Disposing)
        {
            return;
        }
        try
        {
            BeginInvoke(() =>
            {
                _plan = plan;
                _changelog = log;
                ApplyPlanToUi();
            });
        }
        catch (InvalidOperationException)
        {
            // The window went away between the check above and this call. Nothing to update.
        }
    }

    /// <summary>Repaint the update line and the changelog button from the plan we now hold.</summary>
    private void ApplyPlanToUi()
    {
        // Rewrite only the "Update" row, leaving Installed/Build/Available as they were: those come
        // from files on this machine and did not change while the network was being asked.
        var lines = _status.Text.Split('\n');
        for (var i = 0; i < lines.Length; i++)
        {
            if (lines[i].StartsWith("Update", StringComparison.Ordinal))
            {
                lines[i] = "Update           " + UpdateStatus.DescribePlan(_plan);
            }
        }
        _status.Text = string.Join('\n', lines);

        // YELLOW MEANS "THERE IS SOMETHING TO READ ABOUT, AND SOMETHING WILL DOWNLOAD". It is driven
        // by the same plan that produced the line above, so the colour cannot disagree with the
        // text. Unknown stays blue: an unanswered check is not an announcement.
        if (_plan.HasUpdate)
        {
            _changelogBtn.Recolour(LauncherTheme.CautionFace, LauncherTheme.Caution);
        }
        else
        {
            _changelogBtn.Recolour(LauncherTheme.PlayFace, LauncherTheme.PlayBorder);
        }
    }

    // GeneralsX @feature wh1ter0se 12/08/2026 "packaged <date>" out of BUILD.txt, which ships
    // inside every payload. Used only when the installed record has no usable ordinal.
    private static string ReadPackagedDate(string dir)
    {
        try
        {
            var f = Path.Combine(dir, "BUILD.txt");
            if (!File.Exists(f)) return "";
            foreach (var raw in File.ReadLines(f))
            {
                var l = raw.Trim();
                if (!l.StartsWith("packaged", StringComparison.OrdinalIgnoreCase)) continue;
                var i = l.IndexOf(':');
                var v = (i >= 0 ? l[(i + 1)..] : l["packaged".Length..]).Trim();
                return v.Length > 0 ? v : "";
            }
        }
        catch { /* same posture as the state file: never block the launcher over this */ }
        return "";
    }

    // The first-frame estimate, from gxmanifest.txt - the cache GeneralsXUpdate.exe writes into the
    // install directory. Wire format, from the parser in gxlaunch.c - fields split on '|':
    //   0 version | 1 ordinal | 2 minimum | 3 key | 4 relays | 5 base | 6 builds | 7 signature
    // and field 6 is ';'-separated items of "platform,name,hash,size".
    //
    // GeneralsX @bugfix Claude 13/08/2026 THIS IS NO LONGER THE ANSWER, ONLY THE FIRST GUESS, and
    // that demotion is the fix. It used to be the only thing the panel consulted, and it printed
    // "- up to date" whenever the cached document agreed with the install - which it does after
    // every successful run, forever, because the updater writes it at the end of one. The verdict
    // now comes from Apply.CheckAsync, which fetches the CURRENT document; this only fills the pane
    // until that answers, and no longer claims to know that nothing will download.
    //
    // The rule itself moved to UpdateStatus.CacheOnlyVerdict so it can be run on a machine that
    // cannot run WinForms. Two drifts from the updater's real rule were fixed on the way: it
    // ignored the platform filter, and it matched artifacts by name where the updater matches by
    // (platform, name).
    private static (string available, string summary) ReadCacheVerdict(string dir, string installedCommit)
    {
        try
        {
            var f = Path.Combine(dir, "gxmanifest.txt");
            if (!File.Exists(f)) return ("unknown", "checking…");

            var state = Path.Combine(dir, "gxinstalled.txt");
            var installedLines = File.Exists(state) ? File.ReadLines(state) : Enumerable.Empty<string>();
            return UpdateStatus.CacheOnlyVerdict(File.ReadAllText(f), installedLines, installedCommit);
        }
        catch { /* never block the launcher over a cache file */ }
        return ("unknown", "checking…");
    }

    private async Task PlayAsync()
    {
        _play.Enabled = false;
        _options.Enabled = false;
        try
        {
            var dir = OwnedPaths.InstallDir;
            Directory.CreateDirectory(dir);

            SetWork("Checking for updates…", true);
            // ANNOUNCE THE UPDATE BEFORE THE TRANSFER, not during it. The first progress line is the
            // first sign anything is happening, and 67 MB on a slow connection is minutes of a
            // launcher that looks like it is doing nothing to somebody who pressed Play expecting a
            // game. Said once, on the first progress report, because the updater does not announce
            // its decision separately and a per-line message would fight the byte counter.
            var announced = false;
            var progress = new Progress<Apply.Transfer>(t =>
            {
                if (!announced)
                {
                    announced = true;
                    _work.Text = "A new version was found. Downloading it now…";
                }
                if (t.Determinate)
                {
                    _bar.Style = ProgressBarStyle.Continuous;
                    _bar.Value = Math.Clamp((int)Math.Round(t.Fraction * 1000.0), 0, 1000);
                    _work.Text = $"New version — downloading… {t.Done / 1048576.0:0.0} of {t.Total / 1048576.0:0.0} MB";
                }
                else
                {
                    _bar.Style = ProgressBarStyle.Marquee;
                }
            });

            // The update runs EVERY time Play is pressed, before the game starts. A peer whose
            // sourceID does not match the others cannot join them, so "play now, update later" is
            // not a shortcut - it is a guaranteed failed join.
            var result = await Apply.RunUpdaterAsync(dir, checkOnly: false, _ => { },
                CancellationToken.None, progress);

            // GeneralsX @bugfix Claude 12/08/2026 THE PANEL IS NOW STALE, AND IT IS STALE AT EXACTLY
            // THE MOMENT SOMEBODY IS READING IT.
            //
            // RefreshStatusAsync was wired to Shown and to nothing else (see Wire()), so everything
            // it renders - the installed build, its date, what the manifest says is available - was
            // read once when the window opened and never again. The updater has just rewritten both
            // files it reads (gxinstalled.txt and the gxmanifest.txt cache), so from here on the
            // display describes the build that was replaced.
            //
            // It shows on every path where this window survives Play: the "could not install" branch
            // below, the catch, and the already-restarted branch that starts the game with the window
            // still up. Two small file reads and no network - ReadManifestStatus reads the CACHE
            // precisely so this cannot hang - so it is done unconditionally rather than guessing at
            // which branch is about to be taken.
            await RefreshStatusAsync();

            // THE UPDATER JUST REPLACED THIS PROGRAM. The new GeneralsXLauncher.exe is on disk, but
            // Windows keeps running the image it already loaded - so this process is the OLD build
            // and everything the update changed about the launcher is invisible until it restarts.
            // The owner hit exactly this: a successful update that looked like a failed one, because the
            // window on screen was unchanged.
            //
            // The game is NOT started here. Restarting mid-launch would leave the launcher racing a
            // game it just spawned, and the point of the restart is that the next Play runs the new
            // code - so this ends the turn and asks for one more click.
            // ONE RESTART PER CHAIN, ENFORCED HERE. Program.Main records whether this process was
            // itself started by a restart, and a process that was will never ask for another - so a
            // fault that keeps re-triggering the signal costs one extra press, not an endless
            // Play -> download -> restart cycle on somebody's machine. The stop condition has to
            // live in this file: relying on the updater's state file to eventually persist means
            // relying on a disk write in another program to end a loop in this one.
            if (result.LauncherReplaced && !Program.WasRestarted)
            {
                SetWork("Update installed. Restarting the launcher…", true);
                await Task.Delay(900);
                RestartSelf();
                return;
            }
            if (result.LauncherReplaced)
            {
                // Already restarted once and still being told the launcher was replaced. Fall
                // through and START THE GAME on the old image: that is the cosmetic bug this whole
                // change exists to fix, and it is strictly better than never reaching the game.
                SetWork("Update installed. Starting the game — restart the launcher later to " +
                    "pick up its new version.", true);
            }

            if (!Apply.GameInstalled(dir))
            {
                SetWork("Could not install the game. Check your connection and try again.", true);
                return;
            }

            SetWork("Starting…", true);
            // Re-read rather than caching a copy at startup: the options view writes gxlauncher.ini
            // on SAVE, so the file is the only place that knows what the user just chose.
            var launchWith = LauncherSettings.Load();
            Apply.Play(dir, launchWith.Windowed, launchWith.CapFps ? launchWith.FpsCap : null);
            await Task.Delay(1200);
            Close();
        }
        catch (Exception ex)
        {
            SetWork("Something went wrong: " + ex.Message, true);
            // The failure may have happened after the update applied - a launch that threw still
            // leaves a newly updated install behind - so the panel is refreshed here too. Guarded
            // and swallowed: this is the error path, and an exception raised while reporting an
            // exception replaces a useful message with a useless one.
            try
            {
                if (!IsDisposed && !Disposing)
                {
                    await RefreshStatusAsync();
                }
            }
            catch { /* the message above is worth more than the panel */ }
        }
        finally
        {
            _play.Enabled = true;
            _options.Enabled = true;
        }
    }

    /// <summary>
    /// Start the newly installed launcher and close this one.
    ///
    /// THE SINGLE-INSTANCE MUTEX IS THE TRAP, AND THE ORDER IS THE FIX. The replacement is passed
    /// --restarted, which makes it WAIT up to 15s for the mutex instead of refusing (Program.cs), so
    /// the handover race is already covered and this process must NOT surrender the mutex up front.
    /// Releasing first looked harmless and was not: if Process.Start then throws - a freshly written
    /// unsigned exe held by SmartScreen or AV is the ordinary case - this window stays open with
    /// Play re-enabled and no instance guard at all, and the error message invites the user to open
    /// a second copy. Two launchers against one install means two updaters, and do_update wipes and
    /// recreates a SHARED work directory, so they delete each other's download mid-flight.
    ///
    /// So: start first, and only give up the mutex once there is a live process to hand it to. On
    /// any failure the mutex is still held, this window is still usable, and pressing Play again is
    /// what recovers - which is what the messages say, rather than telling somebody to open a
    /// program that just failed to open.
    /// </summary>
    private void RestartSelf()
    {
        var exe = Path.Combine(OwnedPaths.InstallDir, "GeneralsXLauncher.exe");
        try
        {
            if (!File.Exists(exe))
            {
                SetWork("Update installed. Press Play again to start the game.", true);
                return;
            }

            var psi = new ProcessStartInfo(exe)
            {
                WorkingDirectory = OwnedPaths.InstallDir,
                UseShellExecute = true,
            };
            psi.ArgumentList.Add("--restarted");

            var child = Process.Start(psi);
            if (child is null)
            {
                // Never close this window on a start path that produced no process.
                SetWork("Update installed, but the new launcher did not start. Press Play again.", true);
                return;
            }

            Program.ReleaseSingleInstance();
            Close();
        }
        catch (Exception ex)
        {
            // Never retry. A restart that keeps failing must not become a launcher that keeps
            // relaunching itself on somebody's machine.
            SetWork("Update installed, but the launcher could not restart itself (" + ex.Message +
                "). Press Play again to start the game.", true);
        }
    }
}

/// <summary>
/// A flat, bordered, uppercase button. WinForms' own FlatStyle leaves a system-coloured border and
/// a focus rectangle that neither matches this palette nor can be themed away, so it is drawn here.
/// </summary>
internal sealed class BigButton : Control
{
    // NOT readonly since 13/08/2026: the VIEW CHANGELOGS button changes from blue to yellow when a
    // check finds an update, which arrives asynchronously after the button already exists. The
    // alternative - rebuilding the control - would lose its bounds and its place in the tab order
    // for a colour change.
    private Color _face;
    private Color _border;
    private bool _hot;

    /// <summary>Repaint in a different pair. Safe to call before or after the handle exists.</summary>
    public void Recolour(Color face, Color border)
    {
        if (_face == face && _border == border)
        {
            return;
        }
        _face = face;
        _border = border;
        Invalidate();
    }

    /// <summary>
    /// A quiet button: no fill until hovered, dim label, thin border. Exit and Options are ghosts so
    /// that Play is unmistakably the thing to press. Three identically weighted buttons make the
    /// user read all three every time.
    /// </summary>
    public bool Ghost { get; init; }

    /// <summary>The 11pt bold used for ghost buttons. See the note in the constructor.</summary>
    private readonly Font _ghostFont;

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _ghostFont.Dispose();
        }
        base.Dispose(disposing);
    }

    public BigButton(string text, Color face, Color border)
    {
        Text = text;
        _face = face;
        _border = border;
        DoubleBuffered = true;
        Cursor = Cursors.Hand;
        Font = new Font("Segoe UI", 12f, FontStyle.Bold);
        // GeneralsX @bugfix Claude 19/08/2026 W2. OnPaint used to build this font on every repaint -
        // hover, enable, invalidate, resize - and never dispose it. Each one is a GDI font handle;
        // the process has a hard limit on those and dragging the window edge for half a minute was
        // enough to watch the count climb. Built once here, disposed with the control.
        _ghostFont = new Font(Font.FontFamily, 11f, FontStyle.Bold);
        ForeColor = Color.White;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
    }

    protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        var r = new Rectangle(0, 0, Width - 1, Height - 1);

        // GeneralsX @bugfix Claude 19/08/2026 W2. A DISABLED NON-GHOST BUTTON WAS DIMMED TWICE.
        // The ternary below already picks Dim(_face) when !Enabled, and the brush then applied
        // Dim() to that result again - so the disabled Play face rendered at -48 per channel
        // instead of -24, which on #0A1B2E lands close enough to black to read as a painting bug
        // rather than a disabled state. Ghost buttons were NOT double-dimmed, because their branch
        // has no Enabled term at all - which is exactly why the outer Dim() looked correct and
        // survived. Both cases are now resolved in one place, and the brush just uses the answer.
        var face = Ghost
            ? (Enabled ? (_hot ? LauncherTheme.Panel : _face) : Dim(_face))
            : (Enabled ? (_hot ? Lighten(_face, 18) : _face) : Dim(_face));
        using (var b = new SolidBrush(face))
        {
            g.FillRectangle(b, r);
        }
        using (var p = new Pen(Enabled ? _border : Dim(_border), Ghost ? 1f : 2f))
        {
            g.DrawRectangle(p, r);
        }

        var label = Enabled
            ? (Ghost ? (_hot ? LauncherTheme.Text : LauncherTheme.TextDim) : ForeColor)
            : Color.FromArgb(0x55, 0x5C, 0x69);
        TextRenderer.DrawText(g, Text, Ghost ? _ghostFont : Font,
            r, label,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
    }

    private static Color Lighten(Color c, int by) =>
        Color.FromArgb(Math.Min(255, c.R + by), Math.Min(255, c.G + by), Math.Min(255, c.B + by));

    private static Color Dim(Color c) =>
        Color.FromArgb(Math.Max(0, c.R - 24), Math.Max(0, c.G - 24), Math.Max(0, c.B - 24));
}

/// <summary>
/// Dark and NEUTRAL, with blue used only as an accent.
///
/// The first pass took the reference launcher's palette wholesale and tinted every surface navy,
/// which made the whole app read as blue rather than as a dark app with a blue accent. Greys here
/// are deliberately near-neutral (R, G and B within a few points of each other); the only saturated
/// colour in the file is Accent, and it is confined to the rule under the wordmark, the Play
/// button's edge, the selected sidebar row and focus states.
/// </summary>
internal static class LauncherTheme
{
    public static readonly Color Bg = Color.FromArgb(0x14, 0x14, 0x16);
    public static readonly Color Panel = Color.FromArgb(0x1B, 0x1B, 0x1E);
    public static readonly Color Text = Color.FromArgb(0xEC, 0xEE, 0xF0);
    public static readonly Color TextDim = Color.FromArgb(0x96, 0x99, 0x9E);
    public static readonly Color Accent = Color.FromArgb(0x3B, 0x8E, 0xEA);
    public static readonly Color Border = Color.FromArgb(0x2E, 0x2E, 0x33);
    public static readonly Color ButtonFace = Color.FromArgb(0x1B, 0x1B, 0x1E);

    // Play is the one element allowed to carry the accent as a fill, and even then it is a dark
    // slate with an accent EDGE rather than a saturated block.
    public static readonly Color PlayFace = Color.FromArgb(0x1D, 0x2A, 0x38);
    public static readonly Color PlayBorder = Accent;
    public static readonly Color SidebarSel = Color.FromArgb(0x26, 0x26, 0x2B);

    /// <summary>
    /// THE UPDATE-AVAILABLE YELLOW, DERIVED FROM Accent RATHER THAN PICKED.
    ///
    /// Accent is HSL(211.5°, 80.7%, 57.5%). Caution is the SAME saturation and the SAME lightness
    /// with the hue rotated to 45° - so it carries identical visual weight on this ground and needs
    /// no separate tuning to stay legible. CautionFace is the same rotation applied to PlayFace,
    /// which is a desaturated dark form of the very same blue (HSL 211.1°, 31.8%, 16.7%).
    ///
    /// Choosing a yellow by eye is how a palette acquires a colour that is brighter than everything
    /// else and drags the eye off Play; deriving it means the pair differ in hue and in nothing
    /// else. The byte-level mirror (3B8EEA -> EABE3B, 1D2A38 -> 38311D) is a consequence of the
    /// rotation, not the reason for it.
    /// </summary>
    public static readonly Color Caution = Color.FromArgb(0xEA, 0xBE, 0x3B);
    public static readonly Color CautionFace = Color.FromArgb(0x38, 0x31, 0x1D);
}
