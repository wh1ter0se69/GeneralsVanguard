/*
 * GeneralsX @feature Claude 19/08/2026 W5 of the layout restructure.
 *
 * The launcher's own checkbox, replacing MetroCheckBox.
 *
 * MetroCheckBox could not be made DPI-correct, and that is the whole reason the dependency goes.
 * Its tick box is a hardcoded `Rectangle(0, Height/2 - 6, 12, 12)` and its label font is built with
 * GraphicsUnit.Pixel INSIDE OnPaint - a pixel-unit font constructed during painting is invisible to
 * Control.Font, to ScaleControl, and to every DPI mechanism WinForms has. At 150% every checkbox in
 * the app would keep a 12-physical-pixel box and 12-physical-pixel text while everything around it
 * grew by half again.
 *
 * This one sizes everything from the control's own Font, so it scales with the rest of the UI for
 * free once W6 turns DPI scaling on, and needs no per-DPI constants of its own.
 */

using System.Drawing.Drawing2D;

namespace GeneralsX.Loader;

/// <summary>
/// A checkbox that paints itself in the launcher's palette and scales with its font.
///
/// Derives from CheckBox so every caller, and the CheckedChanged wiring, is unchanged - the same
/// reason the factory that builds these returns CheckBox rather than a concrete type.
/// </summary>
internal sealed class GxCheckBox : CheckBox
{
    public GxCheckBox()
    {
        // UserPaint means OnPaint is the only thing that draws; without it the stock control paints
        // its Win32 box first and this draws on top of it.
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint
                 | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
        BackColor = Color.Transparent;
        ForeColor = LauncherTheme.Text;
        Cursor = Cursors.Hand;
        AutoSize = false;
    }

    private bool _hot;

    protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;

        // EVERY DIMENSION COMES FROM THE FONT, which is the entire point of replacing the old one.
        // Font.Height already carries the DPI scale factor, so a 150% display gets a proportionally
        // larger box with no constant to remember and nothing to keep in sync.
        var box = Math.Max(12, (int)Math.Round(Font.Height * 0.82));
        var top = (Height - box) / 2;
        var rect = new Rectangle(1, top, box, box);

        var line = !Enabled ? LauncherTheme.TextDim
                 : Checked ? LauncherTheme.Accent
                 : _hot ? LauncherTheme.Text
                 : LauncherTheme.TextDim;

        if (Checked && Enabled)
        {
            using var fill = new SolidBrush(LauncherTheme.Accent);
            g.FillRectangle(fill, rect);
        }

        using (var pen = new Pen(line, 1.4f))
        {
            g.DrawRectangle(pen, rect);
        }

        if (Checked)
        {
            // A drawn tick rather than a filled square: at a glance a filled square and a disabled
            // control look alike, and this readout is a settings page where the difference matters.
            using var tick = new Pen(Enabled ? LauncherTheme.Bg : LauncherTheme.TextDim, Math.Max(1.6f, box / 7f))
            {
                StartCap = LineCap.Round,
                EndCap = LineCap.Round,
            };
            var l = rect.Left + box * 0.24f;
            var m = rect.Left + box * 0.43f;
            var r = rect.Left + box * 0.78f;
            var yMid = rect.Top + box * 0.52f;
            var yLow = rect.Top + box * 0.72f;
            var yTop = rect.Top + box * 0.30f;
            g.DrawLines(tick, new[]
            {
                new PointF(l, yMid), new PointF(m, yLow), new PointF(r, yTop),
            });
        }

        var textLeft = rect.Right + Math.Max(7, box / 2);
        var textRect = new Rectangle(textLeft, 0, Math.Max(1, Width - textLeft), Height);
        TextRenderer.DrawText(g, Text, Font, textRect,
            Enabled ? ForeColor : LauncherTheme.TextDim,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter
            | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);

        if (Focused)
        {
            using var focus = new Pen(LauncherTheme.Accent) { DashStyle = DashStyle.Dot };
            g.DrawRectangle(focus, new Rectangle(0, 0, Width - 1, Height - 1));
        }
    }
}
