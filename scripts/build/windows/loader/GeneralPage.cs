// GeneralsX @feature wh1ter0se 20/08/2026 The GENERAL page, against real data.
//
// The design (design/OptionsGeneral.dc.html) asks for four things this page did not have: the two
// paths the install actually uses, a REQUIRED row that cannot be switched off, the optional
// archives grouped away from it, and the hotkey-indicator switch.
//
// ONE DELIBERATE DEPARTURE FROM THE DESIGN, AND IT IS A CORRECTION.
//
// The artboard footers the section with "All of these change your data ID". That is false for the
// optional archives and printing it would be worse than saying nothing, because it tells a player
// that ticking ControlBar Pro will stop their friends joining. Measured with
// scripts/test/probe-dataid.sh, both arms on the same install:
//
//     with 340_ControlBarPro*.big x3   data=2505C0C6
//     without them                     data=2505C0C6
//
// Only assetID moved, and assetID is SIMID_WARN_ASSET - a warning, not one of the four tier-1
// gates (engineID / sourceID / dataID / ordinalID) that actually refuse a join. The REQUIRED row
// is the one that carries dataID, because the community patch is where CommandButton.ini comes
// from. So the copy below says that instead.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace GeneralsX.Loader;

internal sealed partial class OptionsPanel
{
    private Keybinds? _keys;

    /// <summary>Bindings, loaded once per page build and shared with the Keybinds page.</summary>
    private Keybinds Keys => _keys ??= Keybinds.Load();

    /// <summary>
    /// A heading that groups the rows under it. Not a Note(): a Note is dim body text and these are
    /// the only thing separating "cannot be switched off" from "safe to change".
    /// </summary>
    private void SectionHead(string text, int gapAbove = 10)
    {
        if (gapAbove > 0)
        {
            Row(new Label { Text = "", AutoSize = false }, gapAbove);
        }
        Row(new Label
        {
            Text = text, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.Text,
            Font = new Font("Segoe UI", 10f, FontStyle.Bold),
            TextAlign = ContentAlignment.MiddleLeft,
        }, 26);
    }

    /// <summary>
    /// label / value / action button, for the two paths.
    ///
    /// The path is rendered with EllipsisPath rather than wrapped: these are long and the useful
    /// half is the END of them, which a wrap puts on a second line and a plain trim removes.
    /// </summary>
    private void PathRow(string label, string value, string buttonText, Action onClick, string? caption)
    {
        var host = new Panel { BackColor = LauncherTheme.Panel, Height = caption is null ? 52 : 70 };

        var lbl = new Label
        {
            Text = label, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 9f),
            Location = new Point(10, 6), Size = new Size(200, 18),
        };
        var val = new Label
        {
            Text = value, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.Text, Font = new Font("Segoe UI", 9.5f),
            Location = new Point(10, 24), Size = new Size(10, 20),
            AutoEllipsis = true,
        };
        host.Controls.Add(lbl);
        host.Controls.Add(val);

        if (caption is not null)
        {
            host.Controls.Add(new Label
            {
                Text = caption, AutoSize = false, UseMnemonic = false,
                ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
                Location = new Point(10, 46), Size = new Size(10, 18), Name = "cap",
            });
        }

        var btn = new Button
        {
            Text = buttonText, FlatStyle = FlatStyle.Flat, UseMnemonic = false,
            ForeColor = LauncherTheme.Text, BackColor = LauncherTheme.ButtonFace,
            Font = new Font("Segoe UI", 8.5f, FontStyle.Bold),
            Size = new Size(84, 26), Name = "btn",
        };
        btn.FlatAppearance.BorderColor = LauncherTheme.Border;
        btn.Click += (_, _) => onClick();
        host.Controls.Add(btn);

        // The host is a wide row, so its width is only known after LayoutRows. Re-flow the children
        // from the host's own resize rather than guessing RowWidth here.
        host.Resize += (_, _) =>
        {
            btn.Location = new Point(Math.Max(10, host.Width - btn.Width - 10), 14);
            var textW = Math.Max(40, host.Width - btn.Width - 30);
            val.Size = new Size(textW, 20);
            if (host.Controls["cap"] is Control c)
            {
                c.Size = new Size(textW, 18);
            }
        };

        Row(host, host.Height, wide: true, gapBelow: 6);
    }

    /// <summary>
    /// One archive/feature row: title, caption, and the file (or "no archive") it corresponds to.
    ///
    /// <paramref name="set"/> null means the row cannot be switched: it renders a REQUIRED pill
    /// instead of a checkbox. That is not styling - a disabled checkbox still reads as "a thing you
    /// could tick if you were allowed", and the community patch is not that. Without it you cannot
    /// join anybody, so it is stated as a fact about the row, not a control you have lost.
    /// </summary>
    private void ArchiveRow(string title, string caption, string meta, bool on, Action<bool>? set)
    {
        var host = new Panel { BackColor = LauncherTheme.Panel, Height = 62 };

        Control head;
        if (set is null)
        {
            head = new Label
            {
                Text = title, AutoSize = false, UseMnemonic = false,
                ForeColor = LauncherTheme.Text, Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                Location = new Point(10, 8), Size = new Size(260, 20),
            };
            var pill = new Label
            {
                Text = "REQUIRED", AutoSize = false, UseMnemonic = false,
                ForeColor = LauncherTheme.Bg, BackColor = LauncherTheme.Accent,
                Font = new Font("Segoe UI", 7.5f, FontStyle.Bold),
                TextAlign = ContentAlignment.MiddleCenter,
                Location = new Point(276, 9), Size = new Size(74, 18),
            };
            host.Controls.Add(pill);
        }
        else
        {
            var cb = new GxCheckBox
            {
                Text = title, Checked = on, AutoSize = false, UseMnemonic = false,
                Location = new Point(8, 6), Size = new Size(340, 24),
            };
            cb.CheckedChanged += (_, _) => set(cb.Checked);
            head = cb;
        }
        host.Controls.Add(head);

        host.Controls.Add(new Label
        {
            Text = caption, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Segoe UI", 8.5f),
            Location = new Point(10, 30), Size = new Size(10, 16), Name = "cap",
        });
        host.Controls.Add(new Label
        {
            Text = meta, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new Font("Consolas", 8f),
            Location = new Point(10, 44), Size = new Size(10, 15), Name = "meta",
        });

        host.Resize += (_, _) =>
        {
            var w = Math.Max(40, host.Width - 20);
            if (host.Controls["cap"] is Control c) c.Size = new Size(w, 16);
            if (host.Controls["meta"] is Control m) m.Size = new Size(w, 15);
        };

        Row(host, host.Height, wide: true, gapBelow: 6);
    }

    /// <summary>
    /// Where Zero Hour actually is, discovered the same way the installer discovers it.
    ///
    /// Live detection rather than a stored string, because the design says "Found automatically"
    /// and a stored path is the one thing that can be confidently wrong after the player moves a
    /// Steam library. A stored override still wins when one has been set.
    /// </summary>
    private static string ZeroHourDir(LauncherSettings s)
    {
        if (!string.IsNullOrEmpty(s.GameDataDir) && GameData.LooksLikeZeroHour(s.GameDataDir))
        {
            return s.GameDataDir!;
        }
        foreach (var c in GameData.Candidates())
        {
            return c.Path;
        }
        return "not found";
    }

    private void BuildGeneralPaths()
    {
        Note("Where it lives, and which extra archives are switched on.");

        var install = OwnedPaths.InstallDir;
        PathRow("Installed to", install, "SHOW", () => ShowInExplorer(install), null);

        var zh = ZeroHourDir(_s);
        PathRow("Zero Hour data", zh, "CHANGE", ChangeZeroHourDir,
            "Found automatically. GeneralsX never writes into it.");

        // GeneralsX @feature Claude 23/09/2026 The engine's own user-data folder, and a way in (owner:
        // an "Open data folder" control in every launcher). This is OwnedPaths.UserDataDir, the
        // resolver every game file this launcher writes goes through, so the folder opened is the one
        // the INSTALLED engine reads: Documents\GeneralsX\GeneralsZH for a current engine, the shared
        // folder for an older one. The macOS launcher has the same row (OptionsGeneral.html).
        var data = OwnedPaths.UserDataDir;
        PathRow("Your data", data, "OPEN", () => OpenDataFolder(data),
            "Your maps, replays, settings and player identity. Put downloaded maps in its Maps folder.");
    }

    /// <summary>
    /// Opens the user-data folder, creating it first. The engine only creates it on its first start,
    /// and a player may want to put a map in before then. OPEN LOG FOLDER does the same for its own
    /// folder.
    /// </summary>
    private static void OpenDataFolder(string path)
    {
        try
        {
            Directory.CreateDirectory(path);
        }
        catch (Exception)
        {
            return;   // an uncreatable folder cannot be shown either; not worth a dialog
        }
        ShowInExplorer(path);
    }

    private static void ShowInExplorer(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", "\"" + path + "\"")
                { UseShellExecute = true });
            }
        }
        catch (Exception)
        {
            // Opening a folder is a convenience; failing to is not worth an error dialog.
        }
    }

    private void ChangeZeroHourDir()
    {
        using var dlg = new FolderBrowserDialog
        {
            Description = "Pick your Zero Hour folder (the one containing INIZH.big)",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false,
        };
        if (dlg.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }
        // Validate with the SAME check the installer uses. Accepting a folder that merely exists
        // would let somebody point this at plain Generals - which has most of the same file names
        // and none of the Zero Hour data - and the failure would surface much later as a game that
        // will not start.
        if (!GameData.LooksLikeZeroHour(dlg.SelectedPath))
        {
            MessageBox.Show(this,
                "That folder does not look like Zero Hour - INIZH.big is not in it.\n\n" +
                "Pick the folder that contains INIZH.big.",
                "Not Zero Hour", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        _s.GameDataDir = dlg.SelectedPath;
        Select(_sel);   // rebuild so the row shows what was just chosen
    }

    /// <summary>
    /// The community patch, stated rather than offered.
    ///
    /// Read from required.txt for the same reason the optional list is read from optional.txt: a
    /// compiled-in name drifts from the payload the first time the set changes, and this is the one
    /// row where being wrong costs somebody every match they try to join.
    /// </summary>
    private void BuildRequiredArchives()
    {
        var names = new List<string>();
        try
        {
            var p = Path.Combine(_patchDir ?? "", "required.txt");
            if (File.Exists(p))
            {
                foreach (var raw in File.ReadAllLines(p))
                {
                    var n = raw.Trim();
                    if (n.Length > 0 && Path.GetFileName(n) == n)
                    {
                        names.Add(n);
                    }
                }
            }
        }
        catch (IOException)
        {
            return;
        }
        if (names.Count == 0)
        {
            return;
        }

        SectionHead("Required to play with anyone else");
        foreach (var n in names)
        {
            var (title, caption) = DescribeRequired(n);
            ArchiveRow(title, caption, n, on: true, set: null);
        }
    }

    private static (string, string) DescribeRequired(string file)
    {
        if (file.StartsWith("500_900_CommunityPatch", StringComparison.OrdinalIgnoreCase))
        {
            return ("Community patch, core INI",
                "The balance and bugfix set every lobby runs. Switch it off and you cannot join anyone, including friends.");
        }
        if (file.Contains("Leikeze", StringComparison.OrdinalIgnoreCase))
        {
            // Kept honest rather than flattering: this archive is required today only because it
            // is where CommandButton.ini comes from - the community patch deliberately ships a
            // ZERO-BYTE copy so a mod can win. Its baked-in hotkey letters are now redundant,
            // because the engine draws them.
            return ("Hotkey pack (supplies CommandButton.ini)",
                "Required because it carries the command-button data. Its painted-on letters are no longer used - the engine draws them.");
        }
        return (file, "Part of the set every lobby runs.");
    }

    /// <summary>
    /// The indicator switch. NOT an archive any more, which is the point.
    ///
    /// It writes ShowHotKeyIndicators into the engine's own keybind file, so it follows the
    /// Keybinds page and needs no download. Applied on the spot: the engine re-reads the file on
    /// init and on reset, so the next match picks it up with no launcher save step.
    /// </summary>
    private void BuildIndicatorSwitch()
    {
        ArchiveRow(
            "Hotkey indicators",
            "Draws each build button's key on its cameo. Follows whatever you set on the Keybinds page.",
            "drawn by the engine, no archive",
            Keys.ShowIndicators,
            v => { Keys.ShowIndicators = v; Keys.Save(); });
    }
}
