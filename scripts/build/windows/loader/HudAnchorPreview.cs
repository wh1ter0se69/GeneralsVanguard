/* Where the top-left readouts sit, drawn rather than described.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * WHY THIS NEEDED A PICTURE AT ALL. "HudAnchorX = 3" is two numbers in a file, and the question a
 * player actually has - "will that block of numbers sit on top of the thing I need to look at?" -
 * cannot be answered by typing a number and restarting the game to find out. The engine reads
 * GXHud.ini exactly once per process (InGameUI.cpp:1031-1081), so the try-it-and-see loop costs a
 * full game launch every attempt. That is the loop this control removes.
 *
 * IT DRAWS THE WHOLE SCREEN, NOT A CROP OF THE CORNER, AND THAT COST SOMETHING TO ACCEPT. A crop of
 * the top-left at 1:1 would show the readouts at true size and would be the more flattering picture.
 * It would also be a lie by omission: the anchor's X ALSO moves the match timer, which the engine
 * insets from the RIGHT edge (drawGameTime, InGameUI.cpp:6932-6936), so a crop that showed only the
 * left corner would leave the user with no idea that the clock on the far side had moved too. The
 * whole screen is the only frame in which both halves of one setting are visible.
 *
 * WHAT THE SCALE COSTS, STATED ON SCREEN RATHER THAN HIDDEN. Fitting a 1920-wide screen into an
 * 800-wide box is roughly 0.42x, so the readout text lands around eight pixels tall. POSITIONS stay
 * exact - every offset, gap and inset is scaled by the same factor - but glyphs below about eight
 * pixels stop being letters and become grey mush, so the text has a floor and the caption says when
 * the floor is in force. Position fidelity is what this control exists for; glyph fidelity is not,
 * and pretending otherwise would make the picture look precise about the wrong thing.
 *
 * THE GROUND IS DELIBERATELY SCHEMATIC. A real screenshot would date, would have to be fetched, and
 * would put a specific map behind a setting that has nothing to do with maps - and PreviewStrip
 * already owns the "real screenshot of a real option" job. This is a diagram, and it reads as one.
 */

namespace GeneralsX.Loader;

/// <summary>
/// A row whose HEIGHT is derived from the width it is given, so the vertical stack cannot be
/// replayed from the heights recorded when the page was built.
///
/// OptionsPanel.LayoutRows used to test for PreviewStrip by type. That was correct while exactly one
/// control behaved this way and became a trap the moment a second one did: a new control that
/// re-derived its height would silently overlap the rows under it, and the failure would look like a
/// painting bug rather than a missing case in a stacker. The marker makes the requirement explicit
/// and makes the third one free.
/// </summary>
internal interface IReflowsToWidth
{
    /// <summary>Re-stack the interior for an available width, setting this control's own Size.</summary>
    void Relayout(int width);
}

internal sealed class HudAnchorPreview : Panel, IReflowsToWidth
{
    // The same floor and ceiling PreviewStrip uses, because these two sit on the same page and a
    // preview that jumped size between sections would read as a layout fault.
    public const int MinBoxWidth = 420;
    public const int MaxBoxWidth = 900;

    /// <summary>
    /// A ceiling on the drawn screen's HEIGHT, which the width ceiling alone does not give.
    /// The box takes the aspect of the resolution being previewed, so a 5:4 screen at 900 wide would
    /// be 720 tall and would push everything below it off the viewport.
    /// </summary>
    public const int MaxBoxHeight = 460;

    /// <summary>Below this many pixels the readouts stop being readable. See the file header.</summary>
    private const int MinTextPx = 8;

    private (int Width, int Height) _screen = (1920, 1080);
    private bool _screenKnown;
    private double _scaler = HudPrefs.LanguageDefaultScaler;
    private int _anchorX = GxHud.DefaultAnchorX;
    private int _anchorY = GxHud.DefaultAnchorY;
    private int _latencySize = HudPrefs.ReadoutDefault;
    private int _fpsSize = HudPrefs.ReadoutDefault;
    private int _sysTimeSize = HudPrefs.ReadoutDefault;
    private int _gameTimeSize = HudPrefs.ReadoutDefault;

    /// <summary>Three lines at 8.5pt, because the third one is conditional. See RefreshCaption.</summary>
    private const int CaptionHeight = 50;

    private readonly Label _caption = new()
    {
        AutoSize = false, UseMnemonic = false, Height = CaptionHeight,
        ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
        TextAlign = ContentAlignment.TopLeft,
    };

    private Rectangle _screenRect = Rectangle.Empty;
    private int _laidOutForWidth = -1;

    public HudAnchorPreview(int width)
    {
        BackColor = LauncherTheme.Bg;
        DoubleBuffered = true;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        Controls.Add(_caption);
        Relayout(width);
    }

    // -------------------------------------------------------------------------------------------
    // What is being previewed. Every setter invalidates, so the picture follows the spin boxes
    // without the page having to remember to ask for a repaint.
    // -------------------------------------------------------------------------------------------

    /// <summary>The resolution the game will run at, and whether that is known or assumed.</summary>
    public void SetScreen((int Width, int Height)? display)
    {
        _screenKnown = display is not null;
        _screen = display ?? (1920, 1080);
        if (_screen.Width <= 0 || _screen.Height <= 0)
        {
            _screenKnown = false;
            _screen = (1920, 1080);
        }
        // The box takes the screen's aspect, so a resolution change is a size change.
        Relayout(Width);
        RefreshCaption();
        Invalidate();
    }

    public double Scaler
    {
        get => _scaler;
        set { _scaler = value; RefreshCaption(); Invalidate(); }
    }

    public int AnchorX
    {
        get => _anchorX;
        set { _anchorX = value; RefreshCaption(); Invalidate(); }
    }

    public int AnchorY
    {
        get => _anchorY;
        set { _anchorY = value; RefreshCaption(); Invalidate(); }
    }

    /// <summary>
    /// The per-readout point sizes from Options.ini, pending edits included.
    ///
    /// THESE ARE NOT DECORATION. A size of 0 is how the engine hides a readout entirely
    /// (postWindowDraw, InGameUI.cpp:4021-4077), and the gates are shared rather than one-per-line:
    /// the resend counter rides on the latency size and the tps counter rides on the fps size. So
    /// setting "Ping / latency" to 0 removes two things from this row, and a preview that kept
    /// drawing them would be showing a layout the game will never produce.
    /// </summary>
    public void SetSizes(int latency, int fps, int sysTime, int gameTime)
    {
        _latencySize = latency;
        _fpsSize = fps;
        _sysTimeSize = sysTime;
        _gameTimeSize = gameTime;
        RefreshCaption();
        Invalidate();
    }

    // -------------------------------------------------------------------------------------------
    // Geometry
    // -------------------------------------------------------------------------------------------

    public void Relayout(int width)
    {
        var avail = Math.Max(200, width);
        _laidOutForWidth = avail;

        var boxW = Math.Clamp(avail - 2, MinBoxWidth, MaxBoxWidth);
        var boxH = (int)Math.Round(boxW * (_screen.Height / (double)_screen.Width));
        if (boxH > MaxBoxHeight)
        {
            boxH = MaxBoxHeight;
            boxW = (int)Math.Round(boxH * (_screen.Width / (double)_screen.Height));
        }
        boxW = Math.Max(80, boxW);
        boxH = Math.Max(60, boxH);

        // One pixel of border on each side, drawn in OnPaint rather than by a child Panel - there is
        // nothing inside the screen that is a control, so a frame control would only be something
        // else to keep in step.
        _screenRect = new Rectangle(1, 1, boxW, boxH);

        _caption.Bounds = new Rectangle(0, boxH + 8, avail, CaptionHeight);
        Size = new Size(avail, boxH + 8 + CaptionHeight);
        RefreshCaption();
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        // Relayout assigns Size, which re-enters here; comparing against the width it was last laid
        // out for is what stops that from recursing. The clamp inside Relayout means the width it
        // settles on may not be the width it was asked for, so the comparison has to be against the
        // settled one.
        if (Width != _laidOutForWidth)
        {
            Relayout(Width);
        }
        else
        {
            Invalidate();
        }
    }

    /// <summary>Preview pixels per game pixel.</summary>
    private float ViewScale => _screen.Width <= 0 ? 1f : _screenRect.Width / (float)_screen.Width;

    /// <summary>
    /// The height in PREVIEW pixels of a readout drawn at <paramref name="pointSize"/>, and whether
    /// the legibility floor had to be applied to get there.
    ///
    /// Three conversions, all of them the engine's own: the resolution scaler
    /// (GlobalLanguage.cpp:344-353, via HudPrefs.AdjustFontSize), points to pixels at a fixed 96 dpi
    /// (render2dsentence.cpp:1506-1507, via HudPrefs.PointsToPixels), and then this preview's scale.
    /// </summary>
    private float TextPx(int pointSize, out bool floored)
    {
        floored = false;
        if (pointSize <= 0)
        {
            return 0f;
        }
        var adjusted = HudPrefs.AdjustFontSize(pointSize, _screen.Width, _screen.Height, _scaler);
        var gamePx = Math.Max(1, HudPrefs.PointsToPixels(adjusted));
        var px = gamePx * ViewScale;
        if (px < MinTextPx)
        {
            floored = true;
            return MinTextPx;
        }
        return px;
    }

    /// <summary>
    /// True when any readout that will be drawn had to be lifted to the legibility floor. A hidden
    /// readout cannot floor - TextPx reports false for a point size of 0 - so this needs no second
    /// test for that.
    /// </summary>
    private bool AnyTextFloored()
    {
        TextPx(_latencySize, out var a);
        TextPx(_fpsSize, out var b);
        TextPx(_sysTimeSize, out var c);
        TextPx(_gameTimeSize, out var d);
        return a || b || c || d;
    }

    /// <summary>
    /// What the picture is, and everything it had to assume or fudge to be one.
    ///
    /// THE TWO NOTES ARE NOT ALTERNATIVES, which an earlier version of this made them. Whether the
    /// text was scaled up for legibility and whether the ping counters are multiplayer-only are
    /// independent facts, and the floor is in force at almost every window size - so making the
    /// second line an either/or quietly hid the multiplayer note nearly always. Both are stated, and
    /// the caption is tall enough for the third line to exist.
    /// </summary>
    private void RefreshCaption()
    {
        var pct = (int)Math.Round(ViewScale * 100f);
        var where = _screenKnown
            ? $"{_screen.Width} x {_screen.Height}"
            : $"{_screen.Width} x {_screen.Height}, assumed — set one on the Graphics page";

        var text = $"Whole screen at {where}, shown at {pct}% — positions are exact.\n" +
                   "Ping and packet-resend counters only appear in a multiplayer match.";
        if (AnyTextFloored())
        {
            text += "\nReadout text is drawn larger than that scale so it stays readable here.";
        }

        _caption.Text = text;
    }

    // -------------------------------------------------------------------------------------------
    // Painting
    // -------------------------------------------------------------------------------------------

    // GeneralsX @refactor Claude 17/08/2026 The colours, the typographic StringFormat, the Tahoma
    // probe and the drop-shadow rule used to be private statics here. HudReadoutPreview draws the
    // same readouts at a different scale, and a second copy of all four would have been four things
    // to keep in step with one engine. They live in HudInk now, with their cites re-read against the
    // tree - every line number in the old block had drifted by about 24.

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(BackColor);

        if (_screenRect.Width <= 0 || _screenRect.Height <= 0)
        {
            return;
        }

        using (var edge = new Pen(LauncherTheme.Border))
        {
            g.DrawRectangle(edge, 0, 0, _screenRect.Width + 1, _screenRect.Height + 1);
        }

        PaintGround(g);

        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;

        // Everything the game draws is clipped by the screen, and the shipped default puts the row
        // one pixel ABOVE the top edge. Without this clip a negative anchor would draw up into the
        // launcher's own page and look like a bug in the preview rather than like the setting doing
        // what it says.
        //
        // Save/Restore rather than reading g.Clip and putting it back: the Clip getter hands out a
        // new Region that the caller owns, and ResetClip would widen the clip to infinity instead of
        // returning it to the invalidated rectangle it arrived as.
        var state = g.Save();
        try
        {
            g.SetClip(_screenRect);
            PaintReadouts(g);
        }
        finally
        {
            g.Restore(state);
        }

        PaintAnchorMarker(g);
    }

    /// <summary>The diagram's ground: a calm gradient plus the retail chrome, hinted, not drawn.</summary>
    private void PaintGround(Graphics g)
    {
        using (var ground = new System.Drawing.Drawing2D.LinearGradientBrush(
                   _screenRect,
                   Color.FromArgb(0x2A, 0x30, 0x26),
                   Color.FromArgb(0x14, 0x18, 0x13),
                   System.Drawing.Drawing2D.LinearGradientMode.ForwardDiagonal))
        {
            g.FillRectangle(ground, _screenRect);
        }

        // The command bar along the bottom and the radar block above its right end - the two pieces
        // of retail chrome a player uses to orient themselves in a picture of their own screen. Kept
        // very low contrast: they are landmarks, not the subject.
        using var chrome = new SolidBrush(Color.FromArgb(0x38, 0x00, 0x00, 0x00));
        var barH = Math.Max(6, (int)Math.Round(_screenRect.Height * 0.11));
        var barW = (int)Math.Round(_screenRect.Width * 0.46);
        g.FillRectangle(chrome,
            _screenRect.Left + ((_screenRect.Width - barW) / 2), _screenRect.Bottom - barH, barW, barH);

        var radar = Math.Max(6, (int)Math.Round(_screenRect.Height * 0.16));
        g.FillRectangle(chrome, _screenRect.Right - radar - 4, _screenRect.Bottom - barH - radar - 4, radar, radar);
    }

    /// <summary>
    /// The readout row, in the engine's order and with the engine's advance rule.
    ///
    /// ORDER AND GATING ARE postWindowDraw's, InGameUI.cpp:4021-4077: latency then resends (both on
    /// the latency size AND multiplayer), fps then its limit then tps (all on the fps size), then the
    /// system clock. The match timer is NOT on this row - it is drawn from the right edge.
    ///
    /// THE ADVANCE RULE HAS ONE ASYMMETRY WORTH COPYING EXACTLY: every readout advances x by its own
    /// width plus a 6px gap, EXCEPT the fps limit, which the engine butts straight up against the fps
    /// number with no gap (:6743-6746) so that "59[60]" reads as one thing.
    /// </summary>
    private void PaintReadouts(Graphics g)
    {
        var scale = ViewScale;
        var face = HudInk.Face("Tahoma");

        var originX = _screenRect.Left + (_anchorX * scale);
        var originY = _screenRect.Top + (_anchorY * scale);
        var gap = GxHud.GapPx * scale;

        var x = originX;

        if (_latencySize > 0)
        {
            var px = TextPx(_latencySize, out _);
            using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);
            x += HudInk.Draw(g, "47ms", font, HudInk.LatencyGood, x, originY) + gap;
            x += HudInk.Draw(g, "0 resent", font, HudInk.ResendGood, x, originY) + gap;
        }

        if (_fpsSize > 0)
        {
            var px = TextPx(_fpsSize, out _);
            using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);
            x += HudInk.Draw(g, "59", font, HudInk.FpsInk, x, originY);
            x += HudInk.Draw(g, "[60]", font, HudInk.FpsLimitInk, x, originY) + gap;
            x += HudInk.Draw(g, "60tps", font, HudInk.LogicRateGood, x, originY) + gap;
        }

        if (_sysTimeSize > 0)
        {
            var px = TextPx(_sysTimeSize, out _);
            using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);
            HudInk.Draw(g, "14:32:07", font, HudInk.SysTimeInk, x, originY);
        }

        // The match timer, inset from the RIGHT by the same anchor X. drawGameTime computes
        // getWidth() - x - width(time) - width(frame) for the clock and getWidth() - x - width(frame)
        // for the trailing frame counter (:6932-6936), so the two are adjacent and the PAIR's right
        // edge is what sits anchorX from the edge of the screen.
        if (_gameTimeSize > 0)
        {
            var px = TextPx(_gameTimeSize, out _);
            using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);
            var wTime = HudInk.Measure(g, "00:12:45", font);
            var wFrame = HudInk.Measure(g, ".17", font);
            var right = _screenRect.Right - (_anchorX * scale);
            HudInk.Draw(g, "00:12:45", font, HudInk.GameTimeInk, right - wFrame - wTime, originY);
            HudInk.Draw(g, ".17", font, HudInk.GameFrameInk, right - wFrame, originY);
        }
    }

    /// <summary>
    /// A crosshair on the anchor point itself, drawn OUTSIDE the clip so it is visible even when the
    /// anchor is above the top edge.
    ///
    /// It earns its place at small offsets. The shipped default is 3 across and -1 up, which at the
    /// scales this control runs at is under two pixels of movement - so the row itself barely stirs
    /// when those numbers change, and without a marker the preview would look broken rather than
    /// faithful. The marker moves visibly because it is drawn at a fixed size.
    /// </summary>
    private void PaintAnchorMarker(Graphics g)
    {
        var scale = ViewScale;
        var px = _screenRect.Left + (_anchorX * scale);
        var py = _screenRect.Top + (_anchorY * scale);

        using var pen = new Pen(Color.FromArgb(0xB0, LauncherTheme.Accent), 1f);
        const float arm = 5f;
        g.DrawLine(pen, px - arm, py, px + arm, py);
        g.DrawLine(pen, px, py - arm, px, py + arm);
    }
}
