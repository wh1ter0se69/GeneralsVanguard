/* Palette and control helpers that sit alongside MetroModernUI.
 *
 * GeneralsX @feature Claude 08/08/2026
 *
 * THIS FILE USED TO CARRY A WRONG CLAIM AND IT IS WORTH RECORDING WHY. It said MetroFramework /
 * MetroModernUI "cannot be used here: those target .NET Framework 4.x and this app is
 * net8.0-windows, so the reference fails at BUILD time". That was asserted, not tested, and it is
 * FALSE: the package restores through the .NETFramework compatibility shim (NU1701) and builds
 * with zero errors against net8.0-windows, because MetroFramework is pure WinForms + System.Drawing
 * and .NET 8 supplies both. The loader now uses it - MetroForm, dark theme, blue accent - as asked.
 *
 * What survives from the old approach, and why: MetroFramework does not style the plain WinForms
 * controls this app builds at runtime (TextBox, NumericUpDown, the flow panels), so the palette and
 * the Apply() walk below still do that work. Without it a MetroForm shows a dark frame around
 * system-white input boxes, which looks more broken than no theme at all.
 *
 * ApplyDarkTitleBar is kept but is no longer load-bearing: MetroForm draws its own frame. It stays
 * for any plain dialog this app puts up (MessageBox owners, the folder browser).
 */

using System.Runtime.InteropServices;

namespace GeneralsX.Loader;

internal static class Theme_
{
    // Windows 11 dark surfaces. Not pure black: a true #000 ground makes the accent glare and
    // hides the elevation difference between the page and the footer.
    public static readonly Color Bg = Color.FromArgb(0x20, 0x20, 0x20);
    public static readonly Color Surface = Color.FromArgb(0x2B, 0x2B, 0x2B);
    public static readonly Color SurfaceAlt = Color.FromArgb(0x32, 0x32, 0x32);
    public static readonly Color Hair = Color.FromArgb(0x3D, 0x3D, 0x3D);

    public static readonly Color Text = Color.FromArgb(0xF0, 0xF0, 0xF0);
    public static readonly Color TextDim = Color.FromArgb(0xA0, 0xA6, 0xAD);

    /// <summary>Windows 11's own dark-mode accent. Legible on 0x202020; the light-mode 0x0078D4 is not.</summary>
    public static readonly Color Accent = Color.FromArgb(0x4C, 0xC2, 0xFF);
    public static readonly Color AccentHover = Color.FromArgb(0x76, 0xD1, 0xFF);
    public static readonly Color AccentPress = Color.FromArgb(0x2F, 0xA8, 0xE8);
    public static readonly Color OnAccent = Color.FromArgb(0x10, 0x18, 0x20);

    public static readonly Color Disabled = Color.FromArgb(0x5A, 0x5A, 0x5A);

    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1 = 19;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    /// <summary>
    /// Dark title bar. Attribute 20 since Windows 10 20H1; 19 on the builds before it. Try both and
    /// ignore failure - on an older Windows the frame stays light and everything still works.
    /// </summary>
    public static void ApplyDarkTitleBar(Form form)
    {
        if (!form.IsHandleCreated)
        {
            form.HandleCreated += (_, _) => ApplyDarkTitleBar(form);
            return;
        }
        var on = 1;
        try
        {
            if (DwmSetWindowAttribute(form.Handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref on, sizeof(int)) != 0)
            {
                DwmSetWindowAttribute(form.Handle, DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1, ref on, sizeof(int));
            }
        }
        catch (DllNotFoundException)
        {
            // No dwmapi: ancient Windows. Not fatal.
        }
    }

    /// <summary>Style a button as the primary (accent-filled) action.</summary>
    public static void Primary(Button b)
    {
        Flat(b);
        b.BackColor = Accent;
        b.ForeColor = OnAccent;
        b.FlatAppearance.MouseOverBackColor = AccentHover;
        b.FlatAppearance.MouseDownBackColor = AccentPress;
        b.Font = new Font(b.Font, FontStyle.Bold);
    }

    /// <summary>Style a button as a secondary action - outline only, so it never competes.</summary>
    public static void Secondary(Button b)
    {
        Flat(b);
        b.BackColor = SurfaceAlt;
        b.ForeColor = Text;
        b.FlatAppearance.BorderColor = Hair;
        b.FlatAppearance.BorderSize = 1;
        b.FlatAppearance.MouseOverBackColor = Color.FromArgb(0x3C, 0x3C, 0x3C);
        b.FlatAppearance.MouseDownBackColor = Color.FromArgb(0x28, 0x28, 0x28);
    }

    private static void Flat(Button b)
    {
        b.FlatStyle = FlatStyle.Flat;
        b.FlatAppearance.BorderSize = 0;
        b.UseVisualStyleBackColor = false;
        b.Cursor = Cursors.Hand;
        b.Padding = new Padding(10, 4, 10, 4);
    }

    /// <summary>
    /// Recolour a control tree. Walks children because WinForms does not inherit BackColor onto
    /// TextBox, Button or ComboBox - they each carry a system colour that stays white otherwise,
    /// which is what makes a half-themed dialog look broken rather than light.
    /// </summary>
    public static void Apply(Control root)
    {
        foreach (Control c in root.Controls)
        {
            switch (c)
            {
                case TextBox tb:
                    tb.BackColor = SurfaceAlt;
                    tb.ForeColor = Text;
                    tb.BorderStyle = BorderStyle.FixedSingle;
                    break;

                case NumericUpDown nud:
                    nud.BackColor = SurfaceAlt;
                    nud.ForeColor = Text;
                    nud.BorderStyle = BorderStyle.FixedSingle;
                    break;

                case Button b:
                    // Buttons opt in explicitly via Primary/Secondary; anything unstyled gets the
                    // secondary look rather than a system-grey slab on a dark ground.
                    if (b.FlatStyle != FlatStyle.Flat)
                    {
                        Secondary(b);
                    }
                    break;

                case CheckBox cb:
                    cb.ForeColor = Text;
                    cb.BackColor = Color.Transparent;
                    break;

                case Label lb:
                    // Leave a deliberately dimmed label dim; only fix the ones still on system ink.
                    if (lb.ForeColor == SystemColors.ControlText || lb.ForeColor == Color.Empty)
                    {
                        lb.ForeColor = Text;
                    }
                    else if (lb.ForeColor == SystemColors.GrayText)
                    {
                        lb.ForeColor = TextDim;
                    }
                    lb.BackColor = Color.Transparent;
                    break;

                case ProgressBar pb:
                    pb.ForeColor = Accent;
                    pb.BackColor = SurfaceAlt;
                    break;

                default:
                    c.ForeColor = Text;
                    break;
            }

            if (c.HasChildren)
            {
                Apply(c);
            }
        }
    }
}
