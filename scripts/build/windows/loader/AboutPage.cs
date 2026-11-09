// GeneralsX @feature wh1ter0se 20/08/2026 The ABOUT page's rows. Data lives in BuildInfo.cs so it
// can be tested without a window - which is how the numbers on this page were checked.
using System;
using System.IO;

namespace GeneralsX.Loader;

internal sealed partial class OptionsPanel
{
    /// <summary>label / value / explanation, the artboard's identity row.</summary>
    private void FactRow(string label, string value, string caption)
    {
        var host = new System.Windows.Forms.Panel
        {
            BackColor = LauncherTheme.Panel, Height = 46,
        };
        host.Controls.Add(new System.Windows.Forms.Label
        {
            Text = label, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new System.Drawing.Font("Segoe UI", 9f),
            Location = new System.Drawing.Point(10, 6), Size = new System.Drawing.Size(170, 18),
        });
        host.Controls.Add(new System.Windows.Forms.Label
        {
            Text = value, AutoSize = false, UseMnemonic = false,
            ForeColor = value.Length == 0 ? LauncherTheme.TextDim : LauncherTheme.Text,
            Font = new System.Drawing.Font("Consolas", 10f, System.Drawing.FontStyle.Bold),
            Location = new System.Drawing.Point(186, 5), Size = new System.Drawing.Size(300, 20),
        });
        host.Controls.Add(new System.Windows.Forms.Label
        {
            Text = caption, AutoSize = false, UseMnemonic = false,
            ForeColor = LauncherTheme.TextDim, Font = new System.Drawing.Font("Segoe UI", 8.5f),
            Location = new System.Drawing.Point(10, 26), Size = new System.Drawing.Size(10, 16),
            Name = "cap",
        });
        host.Resize += (_, _) =>
        {
            if (host.Controls["cap"] is System.Windows.Forms.Control c)
            {
                c.Size = new System.Drawing.Size(Math.Max(60, host.Width - 20), 16);
            }
        };
        Row(host, host.Height, wide: true, gapBelow: 5);
    }

    private void BuildAboutReal()
    {
        Note("What you are running, and who it is built on.");

        var install = OwnedPaths.InstallDir;
        var stamp = BuildInfo.Read(install);

        SectionHead("Who can you play with", gapAbove: 4);

        FactRow("Build",
            stamp is null ? "unknown" : Short(stamp.Commit),
            stamp is null
                ? "No BUILD.txt here, so this is a development copy rather than a packaged install."
                : "The engine you are running. Packaged " + stamp.Packaged + ".");

        // The three join gates, from the engine itself or not at all.
        var sim = BuildInfo.ReadSimId(new[]
        {
            install,
            // GeneralsX @bugfix Claude 23/09/2026 A fifth verbatim copy of the user-data folder.
            // The engine writes the sim-id file into getPath_UserData(), whose leaf comes from the
            // registry, so the literal made this row read "" on any machine that sets it.
            OwnedPaths.UserDataDir,
        });

        FactRow("Simulation digest", sim?.GetValueOrDefault("source") ?? "",
            sim is null
                ? "Seen the first time you run the game. Peers must match this exactly."
                : "Peers must match this exactly, or they refuse each other.");
        FactRow("Engine ID", sim?.GetValueOrDefault("engine") ?? "",
            "Encodes the build's own configuration. A debug build cannot join a release one.");
        FactRow("Data ID", sim?.GetValueOrDefault("data") ?? "",
            "Your archives. A different mod or a loose INI changes it.");

        if (sim is null)
        {
            Note("The three above are read from the engine's own startup line, not recomputed here.");
        }

        SectionHead("Is this the binary that was published");
        var actual = BuildInfo.HashEngine(install);
        if (stamp is null || stamp.ExeSha.Length == 0 || actual is null)
        {
            FactRow("Engine binary", "not checked",
                "generalszh.exe or BUILD.txt is missing, so there is nothing to compare.");
        }
        else
        {
            var ok = string.Equals(actual, stamp.ExeSha, StringComparison.OrdinalIgnoreCase);
            FactRow("Engine binary", ok ? "verified" : "DOES NOT MATCH",
                ok
                    ? "SHA-256 of generalszh.exe matches the one BUILD.txt published."
                    : "generalszh.exe is not the file that was published. Re-download before playing.");
            if (!ok)
            {
                Note("published " + Short(stamp.ExeSha, 16));
                Note("on disk   " + Short(actual, 16));
            }
        }

        SectionHead("Built on");
        Note("The engine        TheSuperHackers/GeneralsGameCode");
        Note("Direct3D over Vulkan   fbraz3/dxvk - Fighter19/dxvk-port");
        Note("This launcher never modifies your Zero Hour installation. It only reads from it.");
        Note("You must own the game.");
    }

    private static string Short(string s, int n = 9) =>
        s.Length <= n ? s : s[..n];
}
