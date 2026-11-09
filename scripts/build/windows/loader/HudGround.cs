/* One real frame of the game, to put the HUD previews on something instead of on nothing.
 *
 * GeneralsX @feature Claude 17/08/2026
 *
 * WHY FLAT BLACK WAS A BUG RATHER THAN A PLAIN BACKGROUND. The message preview asks one question -
 * "will this size and colour be readable while I am playing" - and the game draws those lines over
 * terrain, buildings, shadows and explosions, with nothing behind them but a one-pixel black drop
 * shadow (W3DDisplayString.cpp:158 passes xDrop/yDrop of 1,1). White text on a black rectangle is
 * legible at any size, so a preview on black answers "yes" to every number a player could type. It
 * was not a neutral backdrop; it was the one backdrop that cannot fail.
 *
 * WHERE THE PICTURE CAME FROM. docs/WORKDIR/evidence/lobby-colours-2026-08-12/05-match-radar-full.png
 * - this project's own capture of its own build, already committed, cropped to the terrain band
 * (x 0-756, y 46-424 of that shot) so that none of the game's own HUD is inside it. That crop
 * matters twice: a baked-in "60[60] 60tps" would sit next to the readouts this preview DRAWS and
 * read as a duplicate, and the command bar along the bottom would put chrome behind text that never
 * appears there.
 *
 * IT IS DRAWN AT 1:1 AND NEVER SCALED, WHICH IS THE WHOLE POINT. Terrain in this engine is
 * resolution-independent in the way that matters here - a higher resolution shows MORE map, it does
 * not magnify what is on screen - so one game pixel of that capture is one game pixel at any
 * resolution a player runs. Draw it at 1:1 under text drawn at its true pixel size and the contrast
 * on screen is the contrast they will get. Scale it to fit a box and the comparison quietly becomes
 * a comparison against something else.
 *
 * EMBEDDED RATHER THAN FETCHED, AGAINST THE RULE IN Previews.cs, AND THAT RULE STILL HOLDS FOR WHAT
 * IT COVERS. Those screenshots are per-option data: one per archive, replaced when an archive is
 * replaced, and embedding them would make correcting a picture cost every player the whole ~63.5 MB
 * runtime tier. This is not data. It is the ground the arithmetic is judged against, it does not
 * change when an archive changes, and 33 KB is what it costs. The deciding argument is the failure
 * mode: a fetched ground is a preview that says "unavailable offline" precisely when somebody is
 * sitting in the launcher with no connection deciding whether 14 is too small.
 */

using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Reflection;

namespace GeneralsX.Loader;

/// <summary>The embedded in-game frame, and the one way it is allowed to be painted.</summary>
internal static class HudGround
{
    /// <summary>
    /// Pinned in the .csproj with LogicalName rather than left to the SDK's
    /// RootNamespace-plus-filename convention, so this string and that file agree by construction.
    /// </summary>
    private const string ResourceName = "GeneralsX.Loader.HudGround.jpg";

    /// <summary>
    /// What the previews were on before, kept for the case where the resource will not decode.
    /// A missing ground must degrade to the old picture, not to an exception inside OnPaint - a
    /// throwing paint handler takes the whole options window down.
    /// </summary>
    public static readonly Color Fallback = Color.FromArgb(0x0E, 0x11, 0x0C);

    private static Bitmap? s_frame;
    private static bool s_tried;

    /// <summary>
    /// The frame, decoded once for the life of the process, or null if it could not be.
    ///
    /// PROCESS-LIFETIME ON PURPOSE, and it is the same reasoning as HudAnchorPreview's static
    /// StringFormat: this is repainted on every keystroke in a spin box, and decoding a JPEG per
    /// repaint would be an allocation and a GDI+ decode for a picture that cannot have changed.
    /// Nothing disposes it because nothing may - the previews are rebuilt every time the page is
    /// selected, so an owner-disposes rule would hand every later page a disposed bitmap.
    /// </summary>
    private static Bitmap? Frame
    {
        get
        {
            if (s_tried)
            {
                return s_frame;
            }
            s_tried = true;
            try
            {
                using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName);
                if (stream is null)
                {
                    return null;
                }
                using var decoded = new Bitmap(stream);

                // COPIED RATHER THAN KEPT, and this is not a micro-optimisation. Bitmap(Stream) does
                // not read the stream to completion - it holds it and reads on demand - so returning
                // `decoded` here and letting the `using` close the stream produces a bitmap that
                // throws the first time anything draws it. Blitting into a bitmap we own detaches it.
                //
                // Format32bppPArgb is the format GDI+ blits without converting, which is what a
                // TextureBrush over a repainting control wants.
                var copy = new Bitmap(decoded.Width, decoded.Height, PixelFormat.Format32bppPArgb);
                using (var g = Graphics.FromImage(copy))
                {
                    g.DrawImageUnscaled(decoded, 0, 0);
                }
                s_frame = copy;
            }
            catch (Exception)
            {
                // A build that lost the resource, or a GDI+ that will not decode it. Either way the
                // previews still have to draw.
                s_frame = null;
            }
            return s_frame;
        }
    }

    /// <summary>True when there is a real frame to draw, so a caption can say which one it got.</summary>
    public static bool Available => Frame is not null;

    /// <summary>The frame's height, or 0. Used to pick bands that are actually different.</summary>
    public static int Height => Frame?.Height ?? 0;

    /// <summary>
    /// Fill <paramref name="box"/> with the frame at 1:1, starting <paramref name="bandTop"/> pixels
    /// down it.
    ///
    /// TILED WITH TileFlipXY RATHER THAN STRETCHED. The launcher has no maximum width
    /// (LauncherForm.cs:147 leaves MaximumSize empty), so a maximised window asks for a box far
    /// wider than 756 pixels. Stretching would break the 1:1 rule this file exists for, and plain
    /// tiling would put a hard vertical seam through the terrain every 756 pixels; mirroring at each
    /// tile edge makes the joins continuous, which for ground texture is invisible.
    ///
    /// The bands are what stop five stacked previews from being five copies of the same rock.
    /// </summary>
    public static void Paint(Graphics g, Rectangle box, int bandTop = 0)
    {
        if (box.Width <= 0 || box.Height <= 0)
        {
            return;
        }

        var frame = Frame;
        if (frame is null)
        {
            using var flat = new SolidBrush(Fallback);
            g.FillRectangle(flat, box);
            return;
        }

        // TileFlipXY repeats with a period of TWICE the frame, so that is the modulus that leaves the
        // pattern where the caller asked for it. Taking it against the frame height instead would
        // silently mirror the band vertically for odd multiples.
        var period = frame.Height * 2;
        var top = period > 0 ? ((bandTop % period) + period) % period : 0;

        using var brush = new TextureBrush(frame, WrapMode.TileFlipXY);
        brush.TranslateTransform(box.Left, box.Top - top);
        g.FillRectangle(brush, box);
    }
}
