/* The one picture, and the only thing in the launcher that decodes an image.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * ONE PICTURE, NOT ONE PER OPTION. This replaces PreviewStrip, which was a framed screenshot under
 * every optional archive - five boxes, five placeholders, and on any machine that could not reach
 * the CDN five copies of the sentence "Preview unavailable offline." The owner's instruction was
 * "one picture that changes dynamically for all", and it is the right instruction for a reason
 * beyond taste: what a player is choosing is not five independent things, it is the ONE screen they
 * are going to be looking at, and five separate pictures cannot show how the choices combine.
 *
 * THE PICTURE IS COMPOSED, NOT SELECTED. A base screenshot, then one transparent layer per enabled
 * option painted over it in the order the engine mounts the archives. See PreviewArt.cs for where
 * that order comes from and why it is not a choice made here.
 *
 * WHAT IS NOT SHOWN IS SAID OUT LOUD. An option this build has no art for does not silently leave
 * the picture unchanged as though the option did nothing - the caption names it. That is the whole
 * difference between "this is what you will get" and "this is what you will get, except for these",
 * and the second sentence is the only one this code is ever entitled to say.
 *
 * A LAYER MUST MATCH THE BASE PIXEL FOR PIXEL AND MUST CARRY AN ALPHA CHANNEL, or it is refused.
 * Scaling a layer to fit would put the art somewhere it does not belong - a control bar half a
 * bar-height too high is still a picture of a control bar, and it is a lie about where it will be.
 * The alpha requirement is the other half of the same rule and was added after a cross-model review
 * pointed out that without it an ordinary opaque screenshot - which is exactly what the preview CDN
 * publishes - would be accepted as a layer and would blot out the base and every layer beneath it.
 * There is no partial credit here: a layer that does not fit, or that is not a delta, is treated
 * exactly like a layer that does not exist.
 *
 * DECODING HAPPENS HERE AND NOWHERE ELSE, the same rule PreviewStrip carried, and the catch-all
 * around it is not decoration: GDI+ throws a bare ExternalException on malformed input, and an
 * image that will not decode must degrade to a plain sentence rather than take the options screen
 * down on the way to a game.
 */

using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

namespace GeneralsX.Loader;

internal sealed class GamePreview : Panel, IReflowsToWidth
{
    // The same floor and ceiling PreviewStrip used, for the same reasons: below 420 a screenshot of
    // a HUD stops being readable, and past about 900 the extra pixels tell you nothing new while the
    // rows under it get pushed off the page.
    public const int MinImageWidth = 420;
    public const int MaxImageWidth = 900;

    private readonly PictureBox _shot = new()
    {
        SizeMode = PictureBoxSizeMode.Zoom,          // never distort a screenshot
        BackColor = LauncherTheme.Panel,
    };

    private readonly Label _placeholder = new()
    {
        AutoSize = false, UseMnemonic = false,
        TextAlign = ContentAlignment.MiddleCenter, ForeColor = LauncherTheme.TextDim,
        Font = new Font("Segoe UI", 9f), BackColor = LauncherTheme.Panel,
    };

    private readonly Label _caption = new()
    {
        AutoSize = false, UseMnemonic = false, Height = 20,
        ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
        TextAlign = ContentAlignment.TopLeft,
    };

    private readonly Label _missing = new()
    {
        AutoSize = false, UseMnemonic = false, Height = 20,
        ForeColor = LauncherTheme.Caution, Font = new Font("Segoe UI", 8.5f),
        TextAlign = ContentAlignment.TopLeft,
    };

    private readonly Panel _frame = new();

    private Image? _base;
    private readonly Dictionary<string, Image> _layers = new(StringComparer.OrdinalIgnoreCase);
    private string _baseCaption = "";

    /// <summary>
    /// The base's own pixel size, or Empty when no base decoded. Used by the caller to refuse a
    /// FETCHED layer before downloading it - the document declares its dimensions, so a layer that
    /// could never be composited costs no bytes at all.
    /// </summary>
    public Size CanvasSize => _base is null ? Size.Empty : _base.Size;

    /// <summary>
    /// Is there usable art for this option - decoded, and the right size?
    ///
    /// The caller needs this rather than PreviewArt.For(key).How because a layer can be present in
    /// the manifest and still be unusable, and a row that said "shown in the picture" about art the
    /// picture rejected would be wrong in the one direction that matters.
    /// </summary>
    public bool HasLayer(string key) => _layers.ContainsKey(key);

    public GamePreview(int width)
    {
        BackColor = LauncherTheme.Bg;

        _frame.BackColor = LauncherTheme.Border;
        _shot.Location = new Point(1, 1);
        _placeholder.Location = new Point(1, 1);
        _frame.Controls.Add(_shot);
        _frame.Controls.Add(_placeholder);
        Controls.Add(_frame);
        Controls.Add(_caption);
        Controls.Add(_missing);

        _shot.Visible = false;
        _placeholder.Text = "No preview art shipped with this build.";

        Relayout(width);
    }

    /// <summary>
    /// 16:9 until a base is decoded, then the base's own aspect. A screenshot is not necessarily
    /// 16:9 - the one that ships is 1600x840 - and letterboxing it inside an assumed 16:9 box would
    /// waste the height that PictureBoxSizeMode.Zoom then has to give back.
    /// </summary>
    private double Aspect => _base is null || _base.Width <= 0 || _base.Height <= 0
        ? 9.0 / 16.0
        : (double)_base.Height / _base.Width;

    /// <summary>The width this control was last laid out for. See OnSizeChanged.</summary>
    private int _laidForWidth = -1;

    public void Relayout(int width)
    {
        var avail = Math.Max(200, width);
        var imgW = Math.Clamp(avail - 2, MinImageWidth, MaxImageWidth);
        var imgH = (int)Math.Round(imgW * Aspect);

        // GeneralsX @bugfix Claude 19/08/2026 W6. THE PICTURE WAS SIZED FROM ONE AXIS.
        //
        // imgW came from the available WIDTH and imgH followed from the aspect; the available
        // HEIGHT was never consulted at all. At the options pane's row width of 595 that produced a
        // 593x311 picture inside a 308px viewport - it could not fit on an empty page, let alone
        // under a heading. And because MinImageWidth is a hard floor, shrinking the window made the
        // overflow proportionally WORSE: the picture refused to go below 420 wide while the viewport
        // kept shrinking around it. That is the "the images are too big" complaint, exactly.
        //
        // The captions are measured first because their height does not depend on the picture, only
        // on the width - so the picture can be given whatever vertical budget is actually left.
        var capH = Measure(_caption, avail) + Measure(_missing, avail);
        var viewport = Parent?.ClientSize.Height ?? 0;
        if (viewport > 0)
        {
            // 12 = the 6px above the captions and the 6px trailing margin added below.
            var budget = viewport - capH - 12;
            if (imgH > budget)
            {
                // Shrink to fit, preserving aspect - but not past the readability floor. Below about
                // 420 wide a screenshot of a HUD stops being a screenshot of anything, so where the
                // floor and the viewport genuinely conflict the picture stays legible and the pane
                // scrolls. Scrolling is a real answer; an unreadable picture is not.
                var fitH = Math.Max(1, budget);
                var fitW = (int)Math.Round(fitH / Aspect);
                if (fitW >= MinImageWidth)
                {
                    imgW = Math.Min(imgW, fitW);
                    imgH = (int)Math.Round(imgW * Aspect);
                }
                else
                {
                    imgW = MinImageWidth;
                    imgH = (int)Math.Round(imgW * Aspect);
                }
            }
        }

        _frame.Bounds = new Rectangle(0, 0, imgW + 2, imgH + 2);
        _shot.Size = new Size(imgW, imgH);
        _placeholder.Size = new Size(imgW, imgH);

        // The two caption lines are MEASURED, not assumed to be one line each. "Not shown in this
        // picture:" followed by three labels is comfortably two lines in a 900px window and one in a
        // wide one, and a fixed 20px would cut the second line off in exactly the case where the
        // sentence matters most - the one where the picture is incomplete.
        var y = imgH + 6;
        y += Place(_caption, avail, y);
        y += Place(_missing, avail, y);

        _laidForWidth = width;
        Size = new Size(avail, y + 6);
    }

    /// <summary>What Place would consume for this label at this width, without placing it.</summary>
    private static int Measure(Label lb, int width)
    {
        if (lb.Text.Length == 0)
        {
            return 0;
        }
        return TextRenderer.MeasureText(lb.Text, lb.Font, new Size(width, 0),
            TextFormatFlags.WordBreak).Height + 6 + 2;
    }

    /// <summary>Bound one caption line to its measured height, and report what it consumed.</summary>
    private static int Place(Label lb, int width, int y)
    {
        if (lb.Text.Length == 0)
        {
            lb.Bounds = new Rectangle(0, y, width, 0);
            return 0;
        }
        var h = TextRenderer.MeasureText(lb.Text, lb.Font, new Size(width, 0),
            TextFormatFlags.WordBreak).Height + 6;
        lb.Bounds = new Rectangle(0, y, width, h);
        return h + 2;
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        // Width is assigned by OptionsPanel.LayoutRows; re-stack the interior to match. Guarded
        // against the constructor-time call, and against re-entering from Relayout's own Size
        // assignment by comparing against the width the frame was last laid out for.
        if (!Controls.Contains(_frame))
        {
            return;
        }
        // GeneralsX @bugfix Claude 19/08/2026 W6. Compared against the WIDTH THIS WAS LAID OUT FOR
        // rather than against a width-derived clamp of the frame. Now that the picture's size also
        // depends on the available height, _frame.Width no longer follows from Width alone, so the
        // old test could report "already laid out" for a width it had never seen - or re-enter
        // forever on one it had.
        if (_laidForWidth != Width)
        {
            Relayout(Width);
        }
    }

    /// <summary>
    /// Decode the art. Called once per page build with the embedded set, and again if the network
    /// filled a gap.
    ///
    /// A LAYER THAT DOES NOT DECODE OR DOES NOT FIT IS DROPPED HERE, so everything downstream -
    /// the composite, the caption, and the per-row status - agrees on one set of usable art rather
    /// than each deciding for itself.
    /// </summary>
    public void SetArt(PreviewArt art, IEnumerable<string> keys)
    {
        _baseCaption = art.BaseCaption;

        var newBase = Decode(art.Base);
        if (newBase is not null)
        {
            _base?.Dispose();
            _base = newBase;
        }

        // TWO DIFFERENT ABSENCES, TWO DIFFERENT SENTENCES. "Nothing shipped" is a state somebody can
        // act on by capturing a screenshot; "it shipped and will not decode" is a bug in the build.
        // Printing the first when the second happened would send the reader looking for a missing
        // file that is right there.
        _placeholder.Text = art.Any
            ? "This preview could not be displayed."
            : "No preview art shipped with this build.";

        foreach (var key in keys)
        {
            if (_layers.ContainsKey(key))
            {
                continue;
            }
            var layer = art.For(key);
            if (layer.How != PreviewDepiction.Layer || layer.Bytes is null)
            {
                continue;
            }
            var img = Decode(layer.Bytes);
            if (img is null)
            {
                continue;
            }
            if (_base is null || img.Width != _base.Width || img.Height != _base.Height)
            {
                // Refused rather than scaled - see the header. Disposed immediately: nothing else
                // will ever hold a reference to it.
                img.Dispose();
                continue;
            }
            if (!Image.IsAlphaPixelFormat(img.PixelFormat))
            {
                // A LAYER WITHOUT AN ALPHA CHANNEL IS NOT A LAYER, and this file used to say the
                // opposite - that an opaque full-frame image was legal and "simply replaces
                // everything under it". Cross-model review showed why that is a trap rather than a
                // convenience: an ordinary full-frame screenshot is exactly what the preview CDN
                // publishes, it is opaque, and at the base's size it would be accepted. Two enabled
                // options with one such image each and only the higher-precedence one survives - so
                // the picture would show the other option in whatever state that capture happened
                // to have, confidently and with no way to tell.
                //
                // Requiring alpha makes "this is a delta" something the art itself asserts, and it
                // is not a formality a JPEG can satisfy: JPEG has no alpha channel at all, and a
                // 24-bit PNG decodes to Format24bppRgb. Only a deliberately authored 32-bit PNG
                // gets through.
                img.Dispose();
                continue;
            }
            _layers[key] = img;
        }

        // The aspect may have changed with the base, and the height is derived from it.
        Relayout(Width > 0 ? Width : MinImageWidth);
    }

    /// <summary>
    /// Paint the picture for one selection.
    ///
    /// <paramref name="enabled"/> is every option currently switched on, in any order - the paint
    /// order is imposed here from the filenames, because the caller's order is the order of a text
    /// file and the engine's is the order of a string comparison.
    ///
    /// <paramref name="notShown"/> is the labels of the options whose current state this picture
    /// cannot honestly claim to depict. It is printed under the caption, in the caution colour,
    /// because an unqualified picture is a promise.
    ///
    /// <paramref name="stale"/> says the extra content has been rebuilt since the base was
    /// captured, so the picture is of bytes the player no longer has. Shown rather than hidden, on
    /// the same reasoning the fetched previews used: an old picture of the right options is more
    /// useful than a grey box, and presenting it as current is the only actual failure.
    /// </summary>
    public void Compose(IReadOnlyCollection<string> enabled, IReadOnlyList<string> notShown, bool stale)
    {
        if (_base is null)
        {
            _shot.Visible = false;
            _placeholder.Visible = true;
            _caption.Text = "";
            _missing.Text = "";
            return;
        }

        try
        {
            // Format32bppArgb explicitly: a bitmap created from the base's own format could be
            // indexed or 24-bit, and drawing a transparent layer onto either silently loses the
            // alpha blend - the layer's transparent regions would paint as black.
            var composed = new Bitmap(_base.Width, _base.Height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(composed))
            {
                g.CompositingMode = CompositingMode.SourceOver;
                g.CompositingQuality = CompositingQuality.HighQuality;
                g.InterpolationMode = InterpolationMode.NearestNeighbor;   // never resampled anyway
                var whole = new Rectangle(0, 0, _base.Width, _base.Height);
                g.DrawImage(_base, whole);

                foreach (var key in MountOrder.PaintOrder(enabled))
                {
                    if (_layers.TryGetValue(key, out var img))
                    {
                        g.DrawImage(img, whole);
                    }
                }
            }

            _shot.Image?.Dispose();
            _shot.Image = composed;
            _shot.Visible = true;
            _placeholder.Visible = false;
        }
        catch (Exception)
        {
            _shot.Visible = false;
            _placeholder.Visible = true;
            _placeholder.Text = "This preview could not be displayed.";
            _caption.Text = "";
            _missing.Text = "";
            return;
        }

        _caption.Text = _baseCaption;

        var caveats = new List<string>();
        if (stale)
        {
            caveats.Add("The extra content has been rebuilt since this screenshot was taken, so it " +
                        "may not match what you have.");
        }
        if (notShown.Count > 0)
        {
            caveats.Add("Not shown in this picture: " + string.Join(", ", notShown) + ".");
        }
        _missing.Text = string.Join("\n", caveats);

        // The captions are measured, so their height - and therefore this control's - depends on the
        // text that was just assigned. Re-running the layout here is what keeps the rows below from
        // being overlapped when the list of undepicted options grows onto a second line. It cannot
        // recurse: Relayout assigns Size, and OnSizeChanged only acts when the FRAME's width is
        // stale, which it is not.
        Relayout(Width > 0 ? Width : MinImageWidth);
    }

    private static Image? Decode(byte[]? data)
    {
        if (data is null || data.Length == 0)
        {
            return null;
        }
        try
        {
            // A MemoryStream that outlives the Image, because Image.FromStream keeps a reference to
            // it and disposing it early makes every later paint throw.
            var ms = new MemoryStream(data, writable: false);
            return Image.FromStream(ms);
        }
        catch (Exception)
        {
            return null;
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _shot.Image?.Dispose();
            _shot.Image = null;
            _base?.Dispose();
            _base = null;
            foreach (var img in _layers.Values)
            {
                img.Dispose();
            }
            _layers.Clear();
        }
        base.Dispose(disposing);
    }
}
