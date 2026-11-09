/*
 * GeneralsX @refactor Claude 19/08/2026 W4 of the layout restructure.
 *
 * The vertical-only scroll idiom, in one place. It was written out twice - once in OptionsPanel's
 * constructor and once in ChangelogPanel's, over a two-element array - with the same comment
 * explaining the same non-obvious ordering in both. Two copies of a sequence whose ORDER is the
 * whole point is one copy too many: the failure mode of getting it wrong is not a crash but a
 * horizontal scrollbar that silently clips the first character of every label, which reads as a
 * font or padding problem rather than as scroll configuration.
 */

namespace GeneralsX.Loader;

internal static class ScrollUtil
{
    /// <summary>
    /// Let a panel scroll vertically and never horizontally.
    ///
    /// THE ORDER MATTERS AND IS NOT DECORATIVE. The HorizontalScroll properties only stick while
    /// AutoScroll is off; setting them with AutoScroll already true is silently discarded, and
    /// turning AutoScroll back on afterwards is what keeps the vertical bar we do want. This is the
    /// documented WinForms idiom, and every line of it is load-bearing.
    ///
    /// Extracted verbatim from the two constructors that had it. Deliberately not "improved" while
    /// moving: W4 must render byte-identically, so this is a move, not a rewrite.
    /// </summary>
    public static void VerticalOnly(params Panel[] panels)
    {
        foreach (var p in panels)
        {
            p.AutoScroll = false;
            p.HorizontalScroll.Enabled = false;
            p.HorizontalScroll.Visible = false;
            p.HorizontalScroll.Maximum = 0;
            p.AutoScroll = true;
        }
    }
}
