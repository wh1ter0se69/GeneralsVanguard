/* The five corner readouts, drawn instead of described.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * WHY THIS EXISTS WHEN HudPage's HEADER ARGUED AGAINST IT. That header said the message list gets a
 * preview and the other five do not, because "the other five readouts are corner numbers you glance
 * at; a preview of each would be five boxes of noise around the one that matters". The reasoning was
 * about ATTENTION and it was right about attention. It was wrong about information: a spin box
 * reading 8 tells a player nothing about what 8 looks like, and the number they type is not even the
 * number the game draws - at 2560 wide the engine multiplies it by 2.54. Five numbers with no
 * picture is not a calm page, it is five settings nobody can choose deliberately.
 *
 * WHAT MAKES THIS DIFFERENT FROM THE ANCHOR PREVIEW BELOW IT, because two pictures of the same
 * readouts have to answer different questions or one of them is redundant. HudAnchorPreview draws
 * the WHOLE SCREEN at about 0.42x to answer "where will this block sit", and at that scale it has to
 * lift the text to an 8-pixel legibility floor - so the sizes in it are deliberately NOT truthful.
 * This one draws no positions at all and every glyph at its true pixel size. Position there, size
 * here; neither can do both, and saying so in both captions is what keeps them from looking like
 * they disagree.
 *
 * IT SHOWS THE REAL STRINGS, WHICH IS THE PART THAT IS EASY TO FAKE. Every sample below is the
 * engine's own format string, read off InGameUI.cpp and cited at the line it came from: the fps
 * readout really is a bare "%u" butted straight against a "[%u]" cap with no gap (:6465, :6757,
 * :6767-6770), the latency really is "%ums" with a peer name appended only on the packet router
 * (:6559, :6567), the player list really is label-then-value column pairs with the player's own
 * colour on the name (:6989, :7008, :7039). Inventing "FPS: 60" would have been a preview of a game
 * this is not.
 *
 * ONE SHRINK FACTOR ACROSS ALL FIVE ROWS, NOT ONE PER ROW. A player may type 72, which at 2560 wide
 * is a 243-pixel glyph, and something has to give. Shrinking each row to its own box would keep
 * every row full and destroy the one comparison the page is for - "is the match timer bigger than
 * the clock" - by making every size look the same. One factor keeps the rows honest relative to each
 * other, and the caption says what it was.
 */

using System.Globalization;

namespace GeneralsX.Loader;

/// <summary>
/// How the engine puts a HUD readout on screen: the face, the metrics, the one-pixel drop shadow,
/// and the colours.
///
/// SHARED RATHER THAN COPIED. HudAnchorPreview had all of this as private statics, and a second
/// preview of the same readouts would otherwise have meant a second Tahoma probe, a second
/// StringFormat and a second set of colour constants to drift out of step with the engine
/// independently. There is one engine; there is one description of how it draws.
/// </summary>
internal static class HudInk
{
    /// <summary>
    /// Typographic rather than the default, because rows are laid out by ADDING measured widths.
    /// The default StringFormat pads each side of every run, and that padding would be added again
    /// at each of the six strings on the anchor row, walking it visibly right of where the game puts
    /// it. Static because it wraps a native GDI+ object and these controls repaint on every
    /// keystroke.
    /// </summary>
    public static readonly StringFormat Format = new(StringFormat.GenericTypographic)
    {
        FormatFlags = StringFormatFlags.NoWrap | StringFormatFlags.NoClip | StringFormatFlags.MeasureTrailingSpaces,
    };

    /// <summary>The gap the engine puts between adjacent readouts. InGameUI.cpp:982.</summary>
    public const int GapPx = GxHud.GapPx;

    // The engine's own colours, cited at their assignment sites in
    // GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp. Re-read against the tree on
    // 17/08/2026 - the numbers HudAnchorPreview carried were right and every LINE NUMBER beside them
    // had drifted by about 24, which is exactly how a cite stops being checkable.
    public static readonly Color LatencyGood = Color.FromArgb(0, 255, 0);          // :6582, <= 80 ms
    public static readonly Color LatencyFair = Color.FromArgb(255, 224, 0);        // :6584, <= 180 ms
    public static readonly Color LatencyBad = Color.FromArgb(255, 48, 48);         // :6586, above that
    public static readonly Color LatencyNoSample = Color.FromArgb(173, 216, 255);  // :1356, the "--ms" state
    public static readonly Color ResendGood = Color.FromArgb(0, 255, 0);           // :6705
    public static readonly Color FpsInk = Color.FromArgb(255, 255, 0);             // :1384
    public static readonly Color FpsLimitInk = Color.FromArgb(119, 119, 119);      // :1385
    public static readonly Color LogicRateGood = Color.FromArgb(0, 255, 0);        // :6870
    public static readonly Color SysTimeInk = Color.FromArgb(255, 255, 255);       // :1407
    public static readonly Color GameTimeInk = Color.FromArgb(255, 255, 255);      // :1417
    public static readonly Color GameFrameInk = Color.FromArgb(180, 180, 180);     // :6936, hard-coded
    public static readonly Color ListLabel = Color.FromArgb(125, 124, 122);        // :1425
    public static readonly Color ListValue = Color.FromArgb(253, 251, 251);        // :1426
    public static readonly Color MessageInk1 = Color.FromArgb(255, 255, 255);      // :1262
    public static readonly Color MessageInk2 = Color.FromArgb(180, 180, 180);      // :1263

    /// <summary>The row backdrop behind one player-list line. InGameUI.cpp:1428 and :7028.</summary>
    public const int ListBackdropAlpha = 170;

    /// <summary>
    /// A real player colour rather than an invented one: ColorGold, the first entry in
    /// GeneralsZH/Data/INI/Multiplayer.ini:44-45. The list draws each name in its player's own colour
    /// (InGameUI.cpp:7008, :7039), so the preview needs one, and the palette is loose data in this
    /// tree - there was no need to guess.
    /// </summary>
    public static readonly Color PlayerGold = Color.FromArgb(221, 226, 13);

    private static HashSet<string>? s_installed;

    /// <summary>
    /// <paramref name="wanted"/> if it is installed here, a generic sans-serif otherwise.
    ///
    /// Cached for the process because FontFamily.Families enumerates every installed font and this is
    /// asked once per readout per repaint. On Windows both faces this is called with - Tahoma for the
    /// readouts (InGameUI.cpp:1351) and Arial for the message list (:1266) - are always present; the
    /// fallback is for a launcher being run somewhere unusual, not for a normal box.
    /// </summary>
    public static string Face(string wanted)
    {
        if (s_installed is null)
        {
            var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                foreach (var f in FontFamily.Families)
                {
                    found.Add(f.Name);
                }
            }
            catch (Exception)
            {
                // A machine with an unreadable font collection still has to draw something.
            }
            s_installed = found;
        }
        return s_installed.Contains(wanted) ? wanted : FontFamily.GenericSansSerif.Name;
    }

    public static float Measure(Graphics g, string text, Font font) =>
        g.MeasureString(text, font, PointF.Empty, Format).Width;

    /// <summary>
    /// One readout string with the drop shadow under it, returning the width to advance by.
    ///
    /// THE SHADOW IS OPAQUE BLACK AT EXACTLY ONE PIXEL, because that is what the engine draws:
    /// DisplayString::draw forwards xDrop/yDrop of 1,1 (W3DDisplayString.cpp:158) and every readout's
    /// configured drop colour is opaque black (InGameUI.cpp:1357 and its siblings). The message list
    /// is the one that varies, and only in alpha - it makes the drop black at the message's own
    /// alpha so a fading line fades its shadow with it (:4101-4102) - which this reproduces by taking
    /// the alpha from the ink. A softer shadow was flattering and made text look more legible over
    /// terrain than it is, which is the one thing these previews must not do.
    /// </summary>
    public static float Draw(Graphics g, string text, Font font, Color ink, float x, float y)
    {
        using (var shadow = new SolidBrush(Color.FromArgb(ink.A, 0, 0, 0)))
        {
            g.DrawString(text, font, shadow, x + 1f, y + 1f, Format);
        }
        using (var brush = new SolidBrush(ink))
        {
            g.DrawString(text, font, brush, x, y, Format);
        }
        return Measure(g, text, font);
    }

    /// <summary>
    /// A sample player name that is not somebody's handle committed into a public repository, and is
    /// not shorter than a real one either - a generic "Player" would under-represent the width the
    /// row actually needs. Same source as the message preview's defeat notice.
    /// </summary>
    public static string SampleName()
    {
        var who = Environment.UserName;
        if (string.IsNullOrWhiteSpace(who))
        {
            who = "Player";
        }
        return who.Length > 12 ? who[..12] : who;
    }
}

/// <summary>
/// The five "Other HUD readouts" at the size they will really be drawn, on a real frame of the game.
/// </summary>
internal sealed class HudReadoutPreview : Panel, IReflowsToWidth
{
    private const int Rows = 5;
    private const int RowLatency = 0;
    private const int RowFps = 1;
    private const int RowPlayerList = 2;
    private const int RowSysTime = 3;
    private const int RowGameTime = 4;

    private static readonly string[] Titles =
    {
        "Ping / latency", "Frame rate", "Player list", "System clock", "Match timer",
    };

    /// <summary>Space between the label column and the game frame beside it.</summary>
    private const int LabelGap = 10;

    /// <summary>Breathing room above and below a readout inside its row.</summary>
    private const int RowPadY = 3;

    private const int MinRowHeight = 26;

    /// <summary>
    /// A ceiling on one row, so five of them cannot take the whole viewport.
    ///
    /// SIXTY-FOUR IS NOT A ROUND NUMBER PICKED FOR TIDINESS, it is the smallest ceiling that keeps
    /// the DEFAULT readout at true size on a 4K screen. A default install at 3840 wide draws these at
    /// 29 pt, which is a 39-pixel glyph and a 47-pixel line; a smaller ceiling would have meant a
    /// player who had changed nothing at all opening this page and being told the picture had been
    /// shrunk to 72%. The promise this control makes is "actual size", and a promise that does not
    /// survive the default configuration is not worth making.
    /// </summary>
    private const int MaxRowHeight = 64;

    /// <summary>
    /// What a row is sized to hold at true size: half again the default readout. Ordinary choices -
    /// nudging 8 up to 10 or 12 - then need no shrinking at all, and the caption's "actual size" is
    /// the normal state rather than the lucky one.
    /// </summary>
    private const int RowBudgetPt = (HudPrefs.ReadoutDefault * 3) / 2;

    private const int CaptionGap = 8;
    private const int CaptionHeight = 52;

    private (int Width, int Height) _screen = (1920, 1080);
    private bool _screenKnown;
    private double _scaler = HudPrefs.LanguageDefaultScaler;

    private int _latencySize = HudPrefs.ReadoutDefault;
    private int _fpsSize = HudPrefs.ReadoutDefault;
    private int _playerListSize = HudPrefs.ReadoutDefault;
    private int _sysTimeSize = HudPrefs.ReadoutDefault;
    private int _gameTimeSize = HudPrefs.ReadoutDefault;
    private bool _moneyPerMinute;

    private readonly Font _titleFont = new("Segoe UI", 9f);
    private readonly Font _smallFont = new("Segoe UI", 8.25f);

    private readonly Label _caption = new()
    {
        AutoSize = false, UseMnemonic = false, Height = CaptionHeight,
        ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
        TextAlign = ContentAlignment.TopLeft,
    };

    private int _labelWidth = 160;
    private int _stripX = 170;
    private int _stripW = 300;
    private int _rowHeight = MinRowHeight;
    private int _laidOutForWidth = -1;

    /// <summary>
    /// The factor every readout is multiplied by so the widest and tallest of them fit. 1 means the
    /// picture is at true size, which is the only state the caption is allowed to claim it is.
    /// </summary>
    private float _fit = 1f;

    public HudReadoutPreview(int width)
    {
        BackColor = LauncherTheme.Bg;
        DoubleBuffered = true;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
        Controls.Add(_caption);
        Relayout(width);
    }

    /// <summary>100 when every row is at true size, less when the block had to be shrunk to fit.</summary>
    public int FitPercent => Math.Max(1, (int)Math.Floor(_fit * 100f));

    // -------------------------------------------------------------------------------------------
    // What is being previewed
    // -------------------------------------------------------------------------------------------

    /// <summary>
    /// The resolution the game will run at, and whether that is known or assumed.
    ///
    /// ASSUMES 1920x1080 AND LABELS IT, which is HudAnchorPreview's behaviour rather than the message
    /// preview's. The message preview draws unscaled text when Options.ini has no Resolution, which
    /// is defensible in isolation and confusing on a page where its neighbour assumes one - the two
    /// pictures would be describing different screens without saying which. The assumption is stated
    /// in the caption, which is the only version of it that is not a guess presented as a fact.
    /// </summary>
    public void SetScreen((int Width, int Height)? display)
    {
        _screenKnown = display is not null;
        _screen = display ?? (1920, 1080);
        if (_screen.Width <= 0 || _screen.Height <= 0)
        {
            _screenKnown = false;
            _screen = (1920, 1080);
        }
        // Row height is derived from the resolution, so this is a geometry change and not just a
        // repaint. It happens once per page build, before the row is recorded, exactly as it does
        // for the anchor preview - see the Relayout comment about who re-stacks what.
        Relayout(Width);
    }

    public double Scaler
    {
        get => _scaler;
        set { _scaler = value; Relayout(Width); }
    }

    /// <summary>
    /// The per-readout point sizes from Options.ini, pending edits included. A size of 0 is how the
    /// engine hides a readout (postWindowDraw, InGameUI.cpp:4021-4077), so a 0 row shows the ground
    /// with nothing on it and says "hidden" - which is the answer to "what does 0 do".
    ///
    /// DOES NOT CHANGE THIS CONTROL'S HEIGHT, and that is a requirement rather than a coincidence.
    /// OptionsPanel.LayoutRows re-stacks the rows below this one only when the PANEL's width changes;
    /// nothing re-stacks in response to a spin box. A preview that grew taller as the number grew
    /// would slide under the controls beneath it, and it would look like a painting bug.
    /// </summary>
    public void SetSizes(int latency, int fps, int playerList, int sysTime, int gameTime)
    {
        _latencySize = latency;
        _fpsSize = fps;
        _playerListSize = playerList;
        _sysTimeSize = sysTime;
        _gameTimeSize = gameTime;
        Recompute();
    }

    /// <summary>
    /// Whether the player list carries its income column. The checkbox for this sits on the same
    /// page and until now changed nothing a player could see before starting a match.
    /// InGameUI.cpp:6952 and :7017 gate the "+" column on it.
    /// </summary>
    public bool MoneyPerMinute
    {
        get => _moneyPerMinute;
        set { _moneyPerMinute = value; Recompute(); }
    }

    // -------------------------------------------------------------------------------------------
    // Geometry
    // -------------------------------------------------------------------------------------------

    public void Relayout(int width)
    {
        var avail = Math.Max(200, width);
        _laidOutForWidth = avail;

        _labelWidth = Math.Clamp((int)Math.Round(avail * 0.26), 150, 200);
        _stripX = _labelWidth + LabelGap;
        _stripW = Math.Max(60, avail - _stripX - 1);

        // Derived from the RESOLUTION and the width, never from the chosen sizes - see SetSizes for
        // why that is a requirement and not a preference. The 1.25 turns an em size into the line box
        // the font will actually occupy; measuring it properly would need a Graphics and would make
        // this control's HEIGHT depend on font metrics, which is a worse trade than a constant that
        // is generous by a couple of pixels.
        var budget = HudPrefs.PointsToPixels(
            HudPrefs.AdjustFontSize(RowBudgetPt, _screen.Width, _screen.Height, _scaler));
        _rowHeight = Math.Clamp((int)Math.Ceiling(budget * 1.25) + (RowPadY * 2), MinRowHeight, MaxRowHeight);

        var body = _rowHeight * Rows;
        _caption.Bounds = new Rectangle(0, body + CaptionGap, avail, CaptionHeight);
        Size = new Size(avail, body + CaptionGap + CaptionHeight);

        Recompute();
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        // Relayout assigns Size, which re-enters here. Comparing against the width it last laid out
        // for is what stops that recursing, and the comparison has to be against the SETTLED width
        // because Relayout clamps what it was asked for.
        if (Width != _laidOutForWidth)
        {
            Relayout(Width);
        }
        else
        {
            Invalidate();
        }
    }

    protected override void Dispose(bool disposing)
    {
        // AFTER the base, deliberately. WinForms never owns a Font assigned to a control's Font
        // property, so these three would otherwise leak a GDI handle every time the page is rebuilt -
        // but disposing a child control runs a layout pass on this one, and a layout pass that read a
        // Font we had already disposed would throw inside Dispose, which is the worst place to throw.
        base.Dispose(disposing);

        if (disposing)
        {
            _titleFont.Dispose();
            _smallFont.Dispose();
            _caption.Font.Dispose();
        }
    }

    // -------------------------------------------------------------------------------------------
    // The arithmetic, done once per change rather than once per paint
    // -------------------------------------------------------------------------------------------

    private int[] Sizes() => new[] { _latencySize, _fpsSize, _playerListSize, _sysTimeSize, _gameTimeSize };

    /// <summary>The point size the engine will hand the font library for one readout.</summary>
    private int DrawnPt(int chosen) =>
        HudPrefs.AdjustFontSize(chosen, _screen.Width, _screen.Height, _scaler);

    /// <summary>That size in pixels, which is what this control draws at when nothing is shrunk.</summary>
    private int TruePx(int chosen)
    {
        var pt = DrawnPt(chosen);
        return pt <= 0 ? 0 : Math.Max(1, HudPrefs.PointsToPixels(pt));
    }

    /// <summary>
    /// Find the one factor that makes every visible row fit, then refresh the caption.
    ///
    /// MEASURED ON AN OFF-SCREEN Graphics RATHER THAN DISCOVERED WHILE PAINTING. The message preview
    /// finds its fit inside OnPaint and raises an event so its caption can correct itself, because
    /// its box is fixed and only a repaint knows the answer. Here the answer is knowable the moment a
    /// number changes, and a caption that is right before the repaint cannot be caught claiming
    /// "actual size" underneath text that has just been shrunk.
    /// </summary>
    private void Recompute()
    {
        var fit = 1f;
        var room = Math.Max(4, _rowHeight - (RowPadY * 2));
        var face = HudInk.Face("Tahoma");
        var sizes = Sizes();

        try
        {
            using var probe = new Bitmap(1, 1);
            using var g = Graphics.FromImage(probe);

            for (var row = 0; row < Rows; row++)
            {
                var px = TruePx(sizes[row]);
                if (px <= 0)
                {
                    continue;
                }
                using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);
                var lineH = font.GetHeight(g);
                if (lineH > room)
                {
                    fit = Math.Min(fit, room / lineH);
                }
                var need = RowInkWidth(g, row, font, lineH, HudInk.GapPx, forFit: true);
                if (need > _stripW)
                {
                    fit = Math.Min(fit, _stripW / need);
                }
            }
        }
        catch (Exception)
        {
            // Measuring must never be the thing that breaks the page. True size is the honest
            // default and the row clip below keeps an over-wide readout inside its own strip.
            fit = 1f;
        }

        _fit = Math.Clamp(fit, 0.05f, 1f);
        RefreshCaption();
        Invalidate();
    }

    private void RefreshCaption()
    {
        var where = _screenKnown
            ? $"{_screen.Width} x {_screen.Height}"
            : $"{_screen.Width} x {_screen.Height}, assumed — set one on the Graphics page";
        var factor = HudPrefs.ResolutionScale(_screen.Width, _screen.Height, _scaler);

        var text = FitPercent >= 100
            ? $"Actual size, in game pixels, at {where}."
            : $"Too big for this box — shown at {FitPercent}% of actual size, all five together so they stay comparable.";

        text += "\n" + string.Format(
            CultureInfo.InvariantCulture,
            "Every size is multiplied by {0:0.00}x for this screen, so each row says what it will be drawn at.",
            factor);

        if (!HudGround.Available)
        {
            text += "\nThe game frame behind these could not be loaded, so this is a flat backdrop.";
        }

        _caption.Text = text;
    }

    // -------------------------------------------------------------------------------------------
    // What each row says. One composition, used by both the measuring pass and the painting one -
    // two of them would drift and the row would be laid out to a width nothing was drawn at.
    // -------------------------------------------------------------------------------------------

    /// <summary>
    /// One string of a readout: its ink, the gap the engine leaves after it, and whether it is
    /// allowed to run off the right-hand edge.
    ///
    /// ONLY ONE SEGMENT IS EVER ELASTIC - the player's name - and the reason is that it is the only
    /// one whose length is DATA rather than structure. A 24-character handle is legal and the engine
    /// does not truncate it (InGameUI.cpp:6997), so fitting the box to it would shrink the ping, the
    /// clock and the timer, every one of which fits comfortably, because of somebody's name. The
    /// columns are what the size setting is being judged on; the name is allowed to be clipped by the
    /// edge of the strip, exactly as it would be by the edge of a screen.
    /// </summary>
    private readonly record struct Seg(string Text, Color Ink, float GapAfter, bool MayClip = false);

    private List<Seg> Compose(int row, float lineH, float gap, out bool rightAlign, out bool backdrop)
    {
        rightAlign = false;
        backdrop = false;
        var segs = new List<Seg>(12);

        switch (row)
        {
            case RowLatency:
                // "%ums" (:6559) then the resend counter, which rides the SAME preference and is
                // therefore part of what this row's number controls (:6692, postWindowDraw :4047-4051).
                // Both are shown in their good bands; the caption on the page says they are
                // multiplayer-only.
                segs.Add(new Seg("47ms", HudInk.LatencyGood, gap));
                segs.Add(new Seg("0 resent", HudInk.ResendGood, 0f));
                break;

            case RowFps:
                // "%u" then "[%u]" with NO gap between them (:6767-6770) so that "59[60]" reads as one
                // thing, then "%utps" - which also rides this preference (:6856, :6880-6881).
                segs.Add(new Seg("59", HudInk.FpsInk, 0f));
                segs.Add(new Seg("[60]", HudInk.FpsLimitInk, gap));
                segs.Add(new Seg("60tps", HudInk.LogicRateGood, 0f));
                break;

            case RowPlayerList:
            {
                // Label immediately followed by its value, then a gap of half a line to the next
                // column: labelX += labelWidth + valueWidth + columnGap, with
                // columnGap = round(lineH * 6/12) (:6944, :7022). The name comes last, in the
                // player's own colour (:7039), over a translucent black row (:7028).
                backdrop = true;
                var columnGap = (float)Math.Round(lineH * (6.0 / 12.0));
                segs.Add(new Seg("T", HudInk.ListLabel, 0f));
                segs.Add(new Seg("1", HudInk.ListValue, columnGap));
                segs.Add(new Seg("$", HudInk.ListLabel, 0f));
                segs.Add(new Seg("9500", HudInk.ListValue, columnGap));
                if (_moneyPerMinute)
                {
                    segs.Add(new Seg("+", HudInk.ListLabel, 0f));
                    segs.Add(new Seg("250", HudInk.ListValue, columnGap));
                }
                segs.Add(new Seg("*", HudInk.ListLabel, 0f));
                segs.Add(new Seg("1", HudInk.ListValue, columnGap));
                segs.Add(new Seg("XP", HudInk.ListLabel, 0f));
                segs.Add(new Seg("0", HudInk.ListValue, columnGap));
                segs.Add(new Seg(HudInk.SampleName(), HudInk.PlayerGold, 0f, MayClip: true));
                break;
            }

            case RowSysTime:
                // "%2.2d:%2.2d:%2.2d" from GetLocalTime - 24 hour, zero padded, no date (:6899).
                segs.Add(new Seg("14:32:07", HudInk.SysTimeInk, 0f));
                break;

            default:
                // "%2.2d:%2.2d:%2.2d" then ".%2.2d", the second one always grey whatever GameTimeColor
                // says (:6924, :6928, :6936). Right-aligned because drawGameTime insets from the RIGHT
                // edge of the screen (:6932-6936).
                //
                // THAT ".17" IS NOT RELIABLY "THE FRAME WITHIN THIS SECOND". It is `currentFrame % 30`
                // hard-coded (:6921) while the logic rate is LOGICFRAMES_PER_SECOND, which is 60 under
                // GX_LOGIC_60HZ and 30 otherwise - so on a 60 Hz build it wraps twice a second and
                // never reaches 30. The preview shows the shape, and this comment exists so nobody
                // documents it as something it is not.
                rightAlign = true;
                segs.Add(new Seg("00:12:45", HudInk.GameTimeInk, 0f));
                segs.Add(new Seg(".17", HudInk.GameFrameInk, 0f));
                break;
        }

        return segs;
    }

    /// <summary>
    /// How wide a row is. <paramref name="forFit"/> leaves out the segments that are allowed to be
    /// clipped, so the shrink factor is decided by the row's structure and not by its data.
    /// </summary>
    private float RowInkWidth(Graphics g, int row, Font font, float lineH, float gap, bool forFit)
    {
        var segs = Compose(row, lineH, gap, out _, out _);
        var total = 0f;
        foreach (var s in segs)
        {
            if (forFit && s.MayClip)
            {
                continue;
            }
            total += HudInk.Measure(g, s.Text, font) + s.GapAfter;
        }
        return total;
    }

    // -------------------------------------------------------------------------------------------
    // Painting
    // -------------------------------------------------------------------------------------------

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(BackColor);

        var body = _rowHeight * Rows;
        var strip = new Rectangle(_stripX, 0, _stripW, body);
        if (strip.Width <= 0 || strip.Height <= 0)
        {
            return;
        }

        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;

        var face = HudInk.Face("Tahoma");
        var sizes = Sizes();
        // Different bands of the one frame per row, so five rows are not five copies of the same
        // rock. HudGround tiles with a mirror, so any offset is a valid picture.
        var band = HudGround.Height > 0 ? HudGround.Height / Rows : 0;

        for (var row = 0; row < Rows; row++)
        {
            var rowRect = new Rectangle(_stripX, row * _rowHeight, _stripW, _rowHeight);
            HudGround.Paint(g, rowRect, band * row);
            PaintLabel(g, row, sizes[row]);

            var px = (int)Math.Floor(TruePx(sizes[row]) * _fit);
            if (px < 1)
            {
                continue;       // hidden, or shrunk past a pixel; either way there is nothing to draw
            }

            using var font = new Font(face, px, FontStyle.Bold, GraphicsUnit.Pixel);

            // Clipped to its own row so an over-wide readout cannot bleed into its neighbour. The fit
            // factor should prevent that; the clip is what makes a mistake in the factor look like a
            // readout running off the edge rather than like two rows drawn on top of each other.
            var state = g.Save();
            try
            {
                g.SetClip(rowRect);
                PaintRow(g, row, font, rowRect);
            }
            finally
            {
                g.Restore(state);
            }
        }

        // One border around the game frame as a whole, plus a hairline between rows, so the block
        // reads as one picture cut into five and not as five unrelated pictures.
        using (var edge = new Pen(LauncherTheme.Border))
        {
            g.DrawRectangle(edge, strip.Left - 1, 0, strip.Width + 1, strip.Height);
            for (var row = 1; row < Rows; row++)
            {
                var y = row * _rowHeight;
                g.DrawLine(edge, strip.Left, y, strip.Right - 1, y);
            }
        }
    }

    /// <summary>
    /// The launcher's own half of the row: what this readout is, and what the number will really be
    /// drawn at.
    ///
    /// BOTH NUMBERS, ALWAYS. "8" is the number in the file and is not the size of anything; the point
    /// of the page saying "6 pt in Options.ini -> drawn at 15 pt" for the message list was that the
    /// two differ by about a factor of two. Five readouts with only the file's number beside them
    /// would have repeated exactly the mistake that caption exists to correct.
    /// </summary>
    private void PaintLabel(Graphics g, int row, int chosen)
    {
        var top = row * _rowHeight;
        var mid = top + ((_rowHeight - _titleFont.Height) / 2);

        using (var ink = new SolidBrush(LauncherTheme.Text))
        {
            g.DrawString(Titles[row], _titleFont, ink, 0, mid);
        }

        var note = chosen <= 0 ? "hidden" : $"{chosen} → {DrawnPt(chosen)} pt";
        var w = g.MeasureString(note, _smallFont).Width;
        var noteY = top + ((_rowHeight - _smallFont.Height) / 2);
        using (var dim = new SolidBrush(LauncherTheme.TextDim))
        {
            g.DrawString(note, _smallFont, dim, _labelWidth - w, noteY);
        }
    }

    private void PaintRow(Graphics g, int row, Font font, Rectangle rowRect)
    {
        var lineH = font.GetHeight(g);
        var gap = HudInk.GapPx * _fit;
        var segs = Compose(row, lineH, gap, out var rightAlign, out var backdrop);

        // The FULL width here, clippable segments included, because this is what gets drawn and the
        // player list's backdrop is sized to cover its name too (InGameUI.cpp:7028).
        var total = 0f;
        foreach (var s in segs)
        {
            total += HudInk.Measure(g, s.Text, font) + s.GapAfter;
        }

        var pad = Math.Max(2f, (rowRect.Height - lineH) / 2f);
        var y = rowRect.Top + pad;
        var x = rightAlign
            ? Math.Max(rowRect.Left + 2f, rowRect.Right - 4f - total)
            : rowRect.Left + 4f;

        if (backdrop)
        {
            using var back = new SolidBrush(Color.FromArgb(HudInk.ListBackdropAlpha, 0, 0, 0));
            g.FillRectangle(back, x, y, total, lineH);
        }

        foreach (var s in segs)
        {
            x += HudInk.Draw(g, s.Text, font, s.Ink, x, y) + s.GapAfter;
        }
    }
}
