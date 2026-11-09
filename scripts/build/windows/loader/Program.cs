/* GeneralsX Loader entry point.
 *
 * GeneralsX @feature Claude 08/08/2026
 */

namespace GeneralsX.Loader;

internal static class Program
{
    /// <summary>
    /// The single-instance mutex, kept reachable so the self-restart path can hand it over.
    /// </summary>
    private static Mutex? s_single;

    /// <summary>
    /// True when a previous launcher replaced itself and started this process on its way out.
    ///
    /// It is the loop fuse: a process that was itself restarted never asks for another restart, so
    /// any fault that keeps re-raising the signal costs one extra press of Play instead of an
    /// endless download-and-restart cycle. See LauncherForm.PlayAsync.
    /// </summary>
    public static bool WasRestarted { get; private set; }

    /// <summary>
    /// True while the launcher is photographing itself for --snap.
    ///
    /// GeneralsX @feature Claude 19/08/2026 W0 of the layout restructure. This surface has no UI
    /// tests, so "the rebuild looks identical" has to become a byte comparison rather than a
    /// judgement, and a byte comparison is only worth anything if the same arguments produce the
    /// same pixels twice. Everything that varies between two runs a second apart - the install
    /// state on disk, a fetched changelog, a clock, a network probe - is substituted with a fixed
    /// fixture while this is set.
    ///
    /// It is checked in exactly three places (LauncherForm.RefreshStatusAsync,
    /// ChangelogPanel's document source, NetworkPage's results) and must never gate anything that
    /// affects LAYOUT, only content. A fixture that changes how tall a row is defeats the purpose.
    /// </summary>
    public static bool SnapMode { get; private set; }

    /// <summary>
    /// A throwaway GeneralsXPatch directory used only while --snap is running.
    ///
    /// GeneralsX @feature Claude 19/08/2026 The optional-content section - and with it the one
    /// picture on the GENERAL page - is skipped entirely when no optional archives are installed,
    /// which is the normal state of a build machine. So the control whose sizing rule W6 exists to
    /// fix was never once photographed, and "0 images changed" read as "the fix does nothing" when
    /// it actually meant "the fix was never executed".
    ///
    /// A real directory with real files rather than a substituted list, deliberately: the discovery
    /// path filters entries on File.Exists, so faking the list would skip the very code that decides
    /// whether the picture appears. This exercises it. It lives in the temp directory and the real
    /// install is never touched.
    /// </summary>
    public static string? SnapFixtureDir { get; private set; }

    private static void CreateSnapFixture()
    {
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "gxsnap-fixture");
            Directory.CreateDirectory(dir);
            // Names are fixed, not generated: the picture's caption lists them, so a random name
            // would make every snapshot differ from the last and defeat the byte comparison.
            foreach (var name in new[] { "340_ControlBarProZH.big", "!HotkeysLeikezeZH.big" })
            {
                var f = Path.Combine(dir, name);
                if (!File.Exists(f))
                {
                    File.WriteAllBytes(f, Array.Empty<byte>());
                }
            }
            File.WriteAllLines(Path.Combine(dir, "optional.txt"), new[]
            {
                "340_ControlBarProZH.big|ControlBar Pro|A wider command bar",
                "!HotkeysLeikezeZH.big|Leikeze hotkeys|Letters on the build buttons",
            });
            // GeneralsX @feature wh1ter0se 20/08/2026 The REQUIRED section reads this. Without it
            // the snapshot silently omitted the one row on the page that cannot be switched off,
            // which is precisely the row most worth photographing.
            File.WriteAllLines(Path.Combine(dir, "required.txt"), new[]
            {
                "500_900_CommunityPatch_CoreINI.big",
            });
            SnapFixtureDir = dir;
        }
        catch (IOException)
        {
            // A snapshot without the picture is worth more than no snapshot at all.
            SnapFixtureDir = null;
        }
    }

    /// <summary>
    /// Give up single-instance ownership, for a launcher that is about to start its own replacement
    /// and exit. Without this the replacement finds the mutex held by a process that has not quit
    /// yet and refuses to start - see LauncherForm.RestartSelf.
    /// </summary>

    /// <summary>Value of "--flag value", or null when the flag is absent or last.</summary>
    private static string? ArgValue(string[] args, string flag)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], flag, StringComparison.OrdinalIgnoreCase))
            {
                return args[i + 1];
            }
        }
        return null;
    }

    /// <summary>Parses "1280x720". Null on anything else, including a size no window could hold.</summary>
    private static Size? ParseSize(string? spec)
    {
        if (spec is null)
        {
            return null;
        }
        var parts = spec.Split('x', 'X');
        if (parts.Length != 2
            || !int.TryParse(parts[0], out var w)
            || !int.TryParse(parts[1], out var h)
            || w < 320 || h < 240 || w > 8192 || h > 8192)
        {
            return null;
        }
        return new Size(w, h);
    }

    public static void ReleaseSingleInstance()
    {
        try
        {
            s_single?.ReleaseMutex();
        }
        catch (ApplicationException)
        {
            // Not the owner, or already released. Either way there is nothing to hand over.
        }
        try
        {
            s_single?.Dispose();
        }
        catch (Exception)
        {
            // Never let tidying up stop the restart.
        }
        s_single = null;
    }

    /// <summary>
    /// ONE PROGRAM, ONE JOB. This binary is the LAUNCHER: update, then Play.
    ///
    /// It used to be two programs selected by a `--setup` argument, and the wizard half now lives in
    /// its own project at ../setup as GeneralsX-Setup.exe. They had opposite lifetimes - the wizard
    /// runs once and is then rubbish, the launcher runs every time and must stay installed - so
    /// shipping them together meant the download carried the launcher's whole UI and the install
    /// carried the wizard's forever, in one 66 MB file with two personalities that was impossible to
    /// describe to anybody without mentioning a command-line flag.
    ///
    /// Game detection, the owned-paths list and the theme are still shared, by SOURCE: the setup
    /// project compiles GameInstall.cs and Theme.cs out of this directory. There is one copy of the
    /// Zero Hour detection logic, which is the part that matters.
    /// </summary>
    [STAThread]
    private static void Main(string[] args)
    {
        // GeneralsX @bugfix Claude 28/09/2026 NEVER ON A TEST PROFILE. This loader resolves the owner's
        // user-data folder itself (OwnedPaths.UserDataDir) and writes Options.ini, SagePatch.ini and GXHud.ini
        // there; it does not follow GX_USERDATA_DIR (GameClient/GXUserDataDir.h). A caller who set it wants a
        // test profile this program cannot give, so it stops before anything - the camera defaults below
        // included. The dialog only when stderr is not captured: under a harness a modal box is a hang.
        if (Environment.GetEnvironmentVariable("GX_USERDATA_DIR") is not null)
        {
            const string why = "GeneralsX loader: REFUSED - GX_USERDATA_DIR is set (a test profile) and this " +
                "program does not follow it: it would write the owner's Options.ini. Use gxlauncher, or start " +
                "generalszh.exe directly.";
            Console.Error.WriteLine(why);
            if (!Console.IsErrorRedirected)
            {
                MessageBox.Show(why, "GeneralsX", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            Environment.ExitCode = 4;
            return;
        }

        ApplicationConfiguration.Initialize();

        // A friend who double-clicks this twice should get the window they already have, not a
        // second copy racing the first one to write the same files.
        var single = new Mutex(initiallyOwned: true,
            @"Local\GeneralsX.Loader.7f1c2a90", out var isFirst);
        s_single = single;

        // --restarted means the PREVIOUS launcher replaced itself and started this one on its way
        // out (LauncherForm.RestartSelf). It releases the mutex before starting us, but "before" is
        // a race we do not have to lose: wait for it rather than refusing. Bounded, so a genuinely
        // stuck predecessor still produces the normal message instead of hanging forever.
        WasRestarted = args.Any(a => string.Equals(a, "--restarted", StringComparison.OrdinalIgnoreCase));

        if (!isFirst && WasRestarted)
        {
            try
            {
                isFirst = single.WaitOne(TimeSpan.FromSeconds(15));
            }
            catch (AbandonedMutexException)
            {
                // The predecessor died holding it. That is exactly the handover we are waiting for.
                isFirst = true;
            }
        }

        if (!isFirst)
        {
            MessageBox.Show("GeneralsX is already open.", "GeneralsX",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        // Camera defaults, written once if nobody has written them. Before the window exists, because
        // the engine writes its OWN SagePatch.ini the first time it starts without one and whoever
        // writes first wins - see GameSagePatch.EnsureDefaults.
        GameSagePatch.EnsureDefaults();

        // --snap <dir> photographs every view and exits. See SnapMode.
        var snapDir = ArgValue(args, "--snap");
        SnapMode = snapDir is not null;
        if (SnapMode) CreateSnapFixture();

        // --nettest runs the network check with no window and prints it. See
        // OptionsPanel.RunNetworkTestHeadlessAsync for why that is worth having.
        if (args.Any(a => string.Equals(a, "--nettest", StringComparison.OrdinalIgnoreCase)))
        {
            Environment.ExitCode = OptionsPanel
                .RunNetworkTestHeadlessAsync(OwnedPaths.InstallDir, Console.Out)
                .GetAwaiter().GetResult();
            return;
        }

        var form = new LauncherForm();

        // --size WxH forces the client size before anything is drawn, so one run can cover the
        // whole {size} x {dpi} matrix instead of nine.
        var size = ParseSize(ArgValue(args, "--size"));
        if (size is not null)
        {
            form.ClientSize = size.Value;
        }

        // --options opens straight to the settings view. It exists so the UI can be photographed
        // and reviewed without driving synthetic mouse clicks across a desktop somebody is using.
        //
        // GeneralsX @feature Claude 19/08/2026 --options now takes an optional page index. Before
        // this, page 0 was the only one reachable from a command line, so GRAPHICS, HUD, NETWORK,
        // DIAGNOSTICS and ABOUT could not be photographed at all - five of the eight views this
        // restructure has to prove it did not change.
        var optionsArg = args.FirstOrDefault(a =>
            a.StartsWith("--options", StringComparison.OrdinalIgnoreCase));
        if (optionsArg is not null)
        {
            var page = 0;
            var eq = optionsArg.IndexOf('=');
            if (eq >= 0)
            {
                _ = int.TryParse(optionsArg.AsSpan(eq + 1), out page);
            }
            form.Shown += (_, _) => form.ShowOptionsPage(page);
        }
        else if (args.Any(a => string.Equals(a, "--changelog", StringComparison.OrdinalIgnoreCase)))
        {
            form.Shown += (_, _) => form.ShowChangelog(true);
        }

        if (snapDir is not null)
        {
            form.Shown += (_, _) => form.SnapAllViews(snapDir);
        }

        Application.Run(form);

        // The `using` this used to have went away when the mutex became reachable from the restart
        // path, so release it explicitly. Harmless when RestartSelf already did it - that sets the
        // field to null and this becomes a no-op.
        ReleaseSingleInstance();
    }
}
