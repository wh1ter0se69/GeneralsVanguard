// GeneralsX @feature wh1ter0se 20/08/2026 The KEYBINDS page. design/OptionsKeys.dc.html.
//
// "The letter you press to build each thing. The game has no screen for this." - which is literally
// true: retail welds the letter to the '&' in the localized string and offers no way to change it.
// This page is the only UI for HotKeyManager::getCommandButtonHotKey's override file.
//
// WHAT MAKES A BINDING REAL HERE
//
// Every letter shown is resolved the same way the engine resolves it: the user's override if there
// is one, otherwise the '&' from the CSF. Nothing is invented, and nothing is defaulted in this
// file - CommandData reads the same archives the engine mounts, in the same order.
//
// CLASHES ARE THE POINT, NOT A NICETY
//
// addHotKey refuses a key that another VISIBLE button already owns, and since 20/08/2026 the engine
// draws no letter at all on the button that lost. So two buttons sharing a letter does not merely
// confuse - one of them silently stops showing its key. Scope is per command set, because that is
// the set of buttons on screen together; a letter reused by a different structure is not a clash.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace GeneralsX.Loader;

internal sealed partial class OptionsPanel
{
    private List<BuildSet>? _sets;
    private string _keyFaction = "USA";
    private string? _keySet;
    private bool _keysDirty;

    private List<BuildSet> Sets => _sets ??= CommandData.Load(ArchiveDirs());

    /// <summary>
    /// Where to look for the archives, in the engine's own scan order: the game data first, then
    /// the patch overlay. Order matters - see BigReader.
    /// </summary>
    private IEnumerable<string> ArchiveDirs()
    {
        var zh = ZeroHourDir(_s);
        if (zh != "not found")
        {
            yield return zh;
        }
        yield return _patchDir ?? System.IO.Path.Combine(OwnedPaths.InstallDir, "GeneralsXPatch");
    }

    private void BuildKeybinds()
    {
        Note("The letter you press to build each thing. The game has no screen for this.");

        if (Sets.Count == 0)
        {
            Note("Could not read the game's command data from the archives.");
            Note("Check the Zero Hour folder on the GENERAL page.");
            return;
        }

        BuildFactionStrip();

        var inFaction = Sets.Where(s => s.Faction == _keyFaction).ToList();
        if (inFaction.Count == 0)
        {
            Note("Nothing found for " + _keyFaction + ".");
            return;
        }
        // Land on the Command Center rather than whatever sorts first. "Aircraft Carrier" is a
        // real command set but it is not what anybody opens this page to rebind.
        _keySet ??= (inFaction.FirstOrDefault(x =>
                        x.Structure.Contains("Command Center", StringComparison.OrdinalIgnoreCase))
                    ?? inFaction[0]).SetName;
        var set = inFaction.FirstOrDefault(s => s.SetName == _keySet) ?? inFaction[0];
        _keySet = set.SetName;

        BuildStructurePicker(inFaction, set);

        // Resolve every letter the way the engine will, then find the collisions among them.
        var resolved = set.Buttons.ToDictionary(
            b => b.Command,
            b => Keys.Map.TryGetValue(b.Command, out var o) ? o : b.DefaultKey,
            StringComparer.Ordinal);
        var clashes = Keybinds.Clashes(resolved.Values.Where(v => v.Length > 0));

        SectionHead(set.Structure + " builds", gapAbove: 6);
        Note(clashes.Count == 0
            ? "No clashes. Every letter here is one the engine will actually bind."
            : clashes.Count + (clashes.Count == 1 ? " clash" : " clashes")
              + ": two buttons share a letter, and the engine will draw it on only one of them.");

        foreach (var b in set.Buttons)
        {
            var cur = resolved[b.Command];
            KeyRow(b, cur, clashes.Contains(cur) && cur.Length > 0);
        }
    }

    private void BuildFactionStrip()
    {
        var host = new Panel { Height = 34 };
        var x = 0;
        foreach (var f in Sets.Select(s => s.Faction).Distinct().OrderBy(v => v switch
                 {
                     "USA" => 0, "CHINA" => 1, "GLA" => 2, _ => 3,
                 }))
        {
            var sel = f == _keyFaction;
            var b = new Button
            {
                Text = f, FlatStyle = FlatStyle.Flat, UseMnemonic = false,
                ForeColor = sel ? Color.White : LauncherTheme.TextDim,
                BackColor = sel ? LauncherTheme.SidebarSel : LauncherTheme.Panel,
                Font = new Font("Segoe UI", 9f, sel ? FontStyle.Bold : FontStyle.Regular),
                Location = new Point(x, 2), Size = new Size(88, 28),
            };
            b.FlatAppearance.BorderColor = sel ? LauncherTheme.Accent : LauncherTheme.Border;
            var captured = f;
            b.Click += (_, _) =>
            {
                _keyFaction = captured;
                _keySet = null;         // the old structure belongs to the old faction
                Select(_sel);
            };
            host.Controls.Add(b);
            x += 94;
        }
        Row(host, 34, wide: true, gapBelow: 4);
    }

    /// <summary>
    /// The structure picker.
    ///
    /// A button plus a menu rather than a ComboBox. Two reasons, and the second is the real one:
    /// a dark DropDownList paints its selected text through the system renderer and would not show
    /// it, and DrawToBitmap - which is how --snap photographs this page - does not capture a
    /// ComboBox's content at all. A control that cannot be photographed cannot be reviewed.
    /// </summary>
    private void BuildStructurePicker(List<BuildSet> inFaction, BuildSet current)
    {
        var host = new Panel { Height = 34 };
        host.Controls.Add(new Label
        {
            Text = "Built from", AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9f),
            Location = new Point(0, 8), Size = new Size(74, 20),
        });

        var pick = new Button
        {
            Text = current.Structure + "   \u25be", FlatStyle = FlatStyle.Flat, UseMnemonic = false,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = LauncherTheme.Text, BackColor = LauncherTheme.ButtonFace,
            Font = new Font("Segoe UI", 9f),
            Location = new Point(78, 3), Size = new Size(320, 27),
        };
        pick.FlatAppearance.BorderColor = LauncherTheme.Border;

        var menu = new ContextMenuStrip
        {
            BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text, ShowImageMargin = false,
        };
        foreach (var s in inFaction)
        {
            var item = new ToolStripMenuItem(s.Structure)
            {
                BackColor = LauncherTheme.Panel, ForeColor = LauncherTheme.Text,
            };
            var captured = s.SetName;
            item.Click += (_, _) => { _keySet = captured; Select(_sel); };
            menu.Items.Add(item);
        }
        pick.Click += (_, _) => menu.Show(pick, new Point(0, pick.Height));
        host.Controls.Add(pick);

        host.Controls.Add(new Label
        {
            Text = inFaction.Count + " structures",
            AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
            Location = new Point(406, 8), Size = new Size(120, 18),
        });

        Row(host, 34, wide: true, gapBelow: 6);
    }

    /// <summary>
    /// One build button: its letter, editable, and what it builds.
    ///
    /// The box takes a single character and uppercases it. Anything longer is refused at the
    /// keystroke rather than on save, because the engine silently ignores a multi-character binding
    /// and the player would be left looking at a letter that never fires.
    /// </summary>
    private void KeyRow(BuildButton b, string current, bool clashing)
    {
        var host = new Panel { BackColor = LauncherTheme.Panel, Height = 34 };

        var box = new TextBox
        {
            Text = current, MaxLength = 1, TextAlign = HorizontalAlignment.Center,
            BackColor = clashing ? LauncherTheme.CautionFace : LauncherTheme.ButtonFace,
            ForeColor = clashing ? LauncherTheme.Caution : LauncherTheme.Text,
            BorderStyle = BorderStyle.FixedSingle,
            Font = new Font("Consolas", 11f, FontStyle.Bold),
            Location = new Point(8, 5), Size = new Size(34, 24),
        };
        box.TextChanged += (_, _) =>
        {
            var v = box.Text.Trim().ToUpperInvariant();
            if (v.Length > 1)
            {
                v = v[..1];
            }
            if (box.Text != v)
            {
                box.Text = v;
                box.SelectionStart = v.Length;
            }
            if (v.Length == 0)
            {
                // Cleared means "go back to the game's own letter", which is a real choice and is
                // NOT the same as binding nothing - so the override is removed rather than blanked.
                Keys.Map.Remove(b.Command);
            }
            else
            {
                Keys.Map[b.Command] = v;
            }
            _keysDirty = true;
        };
        host.Controls.Add(box);

        host.Controls.Add(new Label
        {
            Text = b.Label, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.Text, Font = new Font("Segoe UI", 9.5f),
            Location = new Point(52, 8), Size = new Size(230, 20), Name = "lbl",
        });
        host.Controls.Add(new Label
        {
            Text = b.DefaultKey.Length > 0 ? "game default " + b.DefaultKey : "no default",
            AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8f),
            Location = new Point(290, 9), Size = new Size(120, 18), Name = "def",
        });

        host.Resize += (_, _) =>
        {
            if (host.Controls["def"] is Control d)
            {
                d.Location = new Point(Math.Max(290, host.Width - 130), 9);
            }
            if (host.Controls["lbl"] is Control l)
            {
                l.Size = new Size(Math.Max(80, host.Width - 200), 20);
            }
        };

        Row(host, 34, wide: true, gapBelow: 3);
    }
}
