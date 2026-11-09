/* Everything the loader actually writes, plus the hand-off to the updater.
 *
 * GeneralsX @feature Claude 08/08/2026
 *
 * WHY THIS DOES NOT SPEAK THE UPDATE PROTOCOL ITSELF. GeneralsXLauncher.exe already fetches the
 * signed release manifest over UDP, verifies its Ed25519 signature against a compiled-in key,
 * downloads, hashes, stages and applies - and that verifier has been reviewed and differentially
 * tested against OpenSSL. Reimplementing any of it here would produce a SECOND implementation of
 * the same trust decision, in a different language, that has to agree with the first one forever.
 * The two would drift, and the way you find out is a friend running an executable nobody signed.
 *
 * So this file is user experience only: it runs the updater, reads its progress, and starts the
 * game. The trust decision stays in exactly one place.
 *
 * IT NO LONGER WRITES ANYTHING. It used to hold Settings, WriteLauncherIni, WriteRunScript and an
 * Options.ini writer, all of which existed for the setup wizard that shared this binary. The wizard
 * is now its own executable (../setup) and owns its own copies, so these were left here as dead
 * code that duplicated a file-format decision in two places - which is the exact shape of a pair
 * that drifts silently. Deleted rather than kept "in case".
 */

using System.Diagnostics;
using System.Globalization;

namespace GeneralsX.Loader;

internal static class Apply
{
    /// <summary>
    /// The updater this program drives. Named GeneralsXUpdate.exe, NOT GeneralsXLauncher.exe -
    /// that name is this program's own, because "the launcher" is the thing a friend double-clicks.
    /// </summary>
    public const string UpdaterExe = "GeneralsXUpdate.exe";


    /// <summary>
    /// A transfer in flight: bytes received and the total the signed manifest promised.
    /// <paramref name="Total"/> is 0 when the manifest gave no size, which means the caller must
    /// show a marquee rather than invent a denominator.
    /// </summary>
    public readonly record struct Transfer(long Done, long Total)
    {
        public bool Determinate => Total > 0;
        public double Fraction => Total > 0 ? (double)Done / Total : 0.0;
    }

    /// <summary>
    /// Lines the updater emits purely for this parser. They carry no timestamp and are not written
    /// to gxlaunch.log, so they must never reach the user-visible log pane.
    /// </summary>
    private const string ProgressPrefix = "progress: ";

    /// <summary>
    /// The updater says it replaced GeneralsXLauncher.exe - the file on disk is now newer than the
    /// process reading this line. Same swallow-don't-log rule as the progress lines.
    /// </summary>
    private const string RestartLine = "restart: launcher";

    /// <summary>What a run of the updater ended up meaning to the caller.</summary>
    public readonly record struct UpdateResult(int ExitCode, bool LauncherReplaced);

    /// <summary>
    /// Parse one line of updater output. Returns true if it was a machine-readable progress line
    /// and should be SWALLOWED rather than logged.
    ///
    /// Kept separate and internal so it can be unit-tested without spawning a process - the wire
    /// format is a contract between two programs in different languages, and the last time this
    /// pair disagreed about a string the mismatch was invisible until it ran on Windows.
    /// </summary>
    internal static bool TryParseProgress(string line, out Transfer t)
    {
        t = default;
        if (!line.StartsWith(ProgressPrefix, StringComparison.Ordinal))
        {
            return false;
        }
        var rest = line.AsSpan(ProgressPrefix.Length);
        var sp = rest.IndexOf(' ');
        if (sp <= 0)
        {
            return false;
        }
        if (!long.TryParse(rest[..sp], NumberStyles.None, CultureInfo.InvariantCulture, out var done) ||
            !long.TryParse(rest[(sp + 1)..], NumberStyles.None, CultureInfo.InvariantCulture, out var total))
        {
            return false;
        }
        t = new Transfer(done, total);
        return true;
    }

    /// <summary>
    /// Run the real updater and stream its progress. Returns its exit code, or -1 if it is absent.
    ///
    /// The launcher is a /SUBSYSTEM:WINDOWS program so that double-clicking it opens no console,
    /// but redirected output still works - which is exactly how this reads its progress.
    ///
    /// <paramref name="progress"/> receives byte counts during the download. It is raised on a
    /// background thread by Process's output reader, so a WinForms caller must marshal to the UI
    /// thread itself - this method deliberately knows nothing about controls.
    /// </summary>
    public static async Task<UpdateResult> RunUpdaterAsync(string installDir, bool checkOnly,
        Action<string> log, CancellationToken ct, IProgress<Transfer>? progress = null)
    {
        var exe = Path.Combine(installDir, UpdaterExe);
        if (!File.Exists(exe))
        {
            log($"{UpdaterExe} is not installed yet - skipping the update check.");
            return new UpdateResult(-1, false);
        }

        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = installDir,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        if (checkOnly)
        {
            psi.ArgumentList.Add("--check");
        }
        psi.ArgumentList.Add("--no-launch");

        // Written by the output reader thread and read after WaitForExitAsync, which is an ordering
        // barrier for it - but volatile rather than plain, because nothing else here forces the
        // write to be visible to the thread that reads it.
        var replaced = 0;

        using var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
        p.OutputDataReceived += (_, e) =>
        {
            if (e.Data is null)
            {
                return;
            }
            // Progress lines are consumed here and never logged: 867 of them arrive during a 67 MB
            // transfer, which would bury the six lines a human actually needs to read.
            if (TryParseProgress(e.Data, out var t))
            {
                progress?.Report(t);
                return;
            }
            if (string.Equals(e.Data.TrimEnd(), RestartLine, StringComparison.Ordinal))
            {
                Interlocked.Exchange(ref replaced, 1);
                return;
            }
            log(e.Data);
        };
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) { log(e.Data); } };

        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        await p.WaitForExitAsync(ct).ConfigureAwait(false);

        // The launcher's own log is the detailed record; surface where it is rather than trying to
        // reproduce it, because "send me that file" is the only debugging channel that survives
        // contact with a friend three time zones away.
        var logPath = Path.Combine(installDir, "gxlaunch.log");
        if (File.Exists(logPath))
        {
            log($"(full detail in {logPath})");
        }
        return new UpdateResult(p.ExitCode, Volatile.Read(ref replaced) == 1);
    }

    /// <summary>
    /// Ask the updater what Play would download, WITHOUT downloading it.
    ///
    /// GeneralsX @bugfix Claude 13/08/2026 This is the call the launcher never made. The panel used
    /// to describe the install from gxmanifest.txt, the cache the updater leaves behind - which
    /// equals the install after every successful run, so it reported "up to date" until Play fetched
    /// the real manifest and found 60 MB of stale tiers. `--check` fetches and verifies the current
    /// document, changes nothing, starts nothing, and now prints the plan it would execute.
    ///
    /// It also REFRESHES gxmanifest.txt as a side effect (bootstrap_manifest -> cache_save), so the
    /// offline fallback improves every time this runs.
    ///
    /// EVERY FAILURE IS UpdatePlan.Unknown, never "up to date". A missing updater, an old updater
    /// that prints no plan, no network, a hang - all of them mean we do not know, and the caller
    /// says so. This method never throws and never blocks Play.
    /// </summary>
    public static async Task<UpdatePlan> CheckAsync(string installDir, CancellationToken ct = default)
    {
        var exe = Path.Combine(installDir, UpdaterExe);
        if (!File.Exists(exe))
        {
            return UpdatePlan.Unknown;
        }

        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = installDir,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        psi.ArgumentList.Add("--check");

        // A check that never returns must not leave the panel saying "checking…" forever. The
        // updater's own HTTPS timeouts are shorter than this, so reaching it means something is
        // genuinely wedged rather than merely slow.
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));

        var lines = new List<string>();
        try
        {
            using var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.OutputDataReceived += (_, e) =>
            {
                if (e.Data is not null)
                {
                    lock (lines) { lines.Add(e.Data); }
                }
            };
            // stderr is drained but discarded: --check writes its diagnostics to gxlaunch.log, and
            // an unread pipe that fills would deadlock the child.
            p.ErrorDataReceived += (_, _) => { };

            p.Start();
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            try
            {
                await p.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                try { p.Kill(entireProcessTree: true); } catch { /* already gone */ }
                return UpdatePlan.Unknown;
            }

            // A non-zero exit means it could not answer - it fell through to one of its "start the
            // game anyway" paths. Its partial output is not a plan.
            if (p.ExitCode != 0)
            {
                return UpdatePlan.Unknown;
            }
        }
        catch (Exception)
        {
            // Cannot start it at all: not a reason to block anything, and not a reason to claim
            // the install is current.
            return UpdatePlan.Unknown;
        }

        lock (lines)
        {
            return UpdateStatus.ParseCheckOutput(lines);
        }
    }

    // GeneralsX @bugfix Claude 11/08/2026 RunLoginAsync and LoginResult were deleted with the
    // loader's LOG IN button. They shelled out to GeneralsXUpdate.exe --login, which ran an RFC
    // 8628 device flow against /device and /claim - endpoints tools/oauth/worker.js does not serve
    // and has tests asserting 404. The flow could not complete on any machine, ever. Signing in is
    // the ENGINE's loopback flow (/login?p=<port>&n=<nonce>); the updater only REPORTS the state of
    // the assertion file the engine writes, on its "login-state:" line during a normal run.

    /// <summary>The engine itself. Absent until an update has actually delivered it.</summary>
    public const string GameExe = "generalszh.exe";

    public static bool GameInstalled(string installDir) =>
        File.Exists(Path.Combine(installDir, GameExe));

    /// <summary>
    /// Launch the game through the same script the shortcuts use.
    ///
    /// <paramref name="windowed"/> becomes "-win", which is the engine's own flag: CommandLine.cpp
    /// maps "-win" to parseWin, which sets m_windowed. It is handed to GeneralsX.cmd rather than to
    /// generalszh.exe directly, because the script is what knows CNC_GENERALS_ZH_PATH and the
    /// working directory the dataID is computed from - so the script has to forward its arguments
    /// for this to reach the engine at all. Nothing is passed when the setting is off, rather than
    /// an explicit "windowed off" flag, because the engine has no such flag: fullscreen is what you
    /// get by not asking for a window.
    ///
    /// UseShellExecute stays on because .NET cannot start a .cmd without a shell. ArgumentList is
    /// still honoured on that path - Process.StartWithShellExecuteEx builds the parameter string
    /// with ProcessStartInfo.BuildArguments, the same method the CreateProcess path uses.
    /// </summary>
    public static void Play(string installDir, bool windowed, int? fpsCap)
    {
        var cmd = Path.Combine(installDir, "GeneralsX.cmd");
        var psi = new ProcessStartInfo(cmd)
        {
            WorkingDirectory = installDir,
            UseShellExecute = true,
        };
        if (windowed)
        {
            psi.ArgumentList.Add("-win");
        }
        // "-fps N" is the engine's own flag: CommandLine.cpp maps it to parseFPSLimit. Same one-hop
        // route as -win, so it depends on GeneralsX.cmd forwarding %* just as that does.
        //
        // Until this existed, CapFps and FpsCap were written to gxlauncher.ini and read back by the
        // options screen and by NOTHING ELSE - the frame cap was a control that changed a number in a
        // file no other code opened. Exactly the shape the Windowed checkbox was in.
        if (fpsCap is { } n && n > 0)
        {
            psi.ArgumentList.Add("-fps");
            psi.ArgumentList.Add(n.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        Process.Start(psi);
    }
}
