// GeneralsX @feature wh1ter0se 20/08/2026 The ABOUT page. design/OptionsAbout.dc.html.
//
// The brief for this page was "measured numbers, not invented ones", and the artboard is itself
// the reason: its Build reads d64b81cd9, which is REAL - it is the commit in this install's
// BUILD.txt - while its Data ID 9C41B0E2 and Engine ID 0x2A17 match nothing this engine produces.
// Measured on 20/08/2026 the live values were dataID=2505C0C6 and engineID=562D4C69, and engineID
// is 32 bits, not the 16 the artboard shows. Two of the four numbers were placeholders.
//
// So this page shows a number ONLY where it can point at where the number came from:
//
//   Build, packaged date, expected exe hash   BUILD.txt, written by the packager
//   Binary hash                               computed HERE, from the exe on disk, and compared
//   sourceID / engineID / dataID              the engine's own [SIMID] line, if it has been seen
//
// The last group is deliberately blank rather than guessed when no run has been observed. Those
// three are the tier-1 join gates; a wrong value on this page would be read as "here is why your
// friend cannot join" and would send somebody hunting a fault that is not there.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;

namespace GeneralsX.Loader;

/// <param name="UserData">
/// GeneralsX @feature Claude 23/09/2026 Where the packaged engine keeps its user data, as the packager
/// stamps it (package-windows-share.ps1). Empty for an engine packaged before the engine moved to
/// Documents\GeneralsX\GeneralsZH, which still uses the folder it shares with retail Zero Hour and
/// Generals Online. <see cref="OwnedPaths.UserDataDir"/> decides on it.
/// </param>
internal sealed record BuildStamp(string Commit, string ExeSha, string Packaged, string UserData = "");

internal static class BuildInfo
{
    /// <summary>BUILD.txt as the packager wrote it. Null when this is not a packaged install.</summary>
    public static BuildStamp? Read(string installDir)
    {
        try
        {
            var p = Path.Combine(installDir, "BUILD.txt");
            if (!File.Exists(p))
            {
                return null;
            }
            string commit = "", sha = "", packaged = "", userData = "";
            foreach (var raw in File.ReadAllLines(p))
            {
                var line = raw.Trim();
                var c = line.IndexOf(':');
                if (c <= 0)
                {
                    continue;
                }
                var k = line[..c].Trim();
                var v = line[(c + 1)..].Trim();
                if (k.StartsWith("built from commit", StringComparison.OrdinalIgnoreCase)) commit = v;
                else if (k.StartsWith("exe sha256", StringComparison.OrdinalIgnoreCase)) sha = v;
                else if (k.StartsWith("packaged", StringComparison.OrdinalIgnoreCase)) packaged = v;
                else if (k.StartsWith("user data", StringComparison.OrdinalIgnoreCase)) userData = v;
            }
            return commit.Length == 0 && sha.Length == 0 ? null : new BuildStamp(commit, sha, packaged, userData);
        }
        // GeneralsX @bugfix Claude 23/09/2026 UnauthorizedAccessException too. It is not an
        // IOException, and this is no longer read only by the About page: OwnedPaths.UserDataDir
        // reads it on every access, so an unreadable BUILD.txt must mean "no stamp", not a crash.
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>
    /// SHA-256 of the shipped engine, computed now.
    ///
    /// This is the check BUILD.txt tells the player to run by hand ("certutil -hashfile"). Doing it
    /// here is the difference between printing a hash and CHECKING one: a mismatch means the binary
    /// on disk is not the binary that was packaged, which is exactly the case where every peer
    /// refusing to join has a cause you can name.
    /// </summary>
    public static string? HashEngine(string installDir)
    {
        try
        {
            var exe = Path.Combine(installDir, "generalszh.exe");
            if (!File.Exists(exe))
            {
                return null;
            }
            using var fs = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var sha = SHA256.Create();
            return Convert.ToHexString(sha.ComputeHash(fs));
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>
    /// The engine's own identity line, if any log has one.
    ///
    /// Parsed rather than recomputed on purpose. Reproducing the digest in C# would mean a second
    /// implementation of the thing whose entire job is to be identical everywhere, and the first
    /// time the two drifted this page would confidently report a number no peer agrees with.
    /// </summary>
    public static Dictionary<string, string>? ReadSimId(IEnumerable<string> logDirs)
    {
        foreach (var dir in logDirs)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
            {
                continue;
            }
            foreach (var f in Directory.EnumerateFiles(dir, "*.log", SearchOption.TopDirectoryOnly))
            {
                try
                {
                    string? last = null;
                    foreach (var line in File.ReadLines(f))
                    {
                        if (line.Contains("[SIMID] local valid", StringComparison.Ordinal))
                        {
                            last = line;
                        }
                    }
                    if (last is null)
                    {
                        continue;
                    }
                    var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    foreach (var tok in last.Split(new[] { ' ', '|' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        var eq = tok.IndexOf('=');
                        if (eq > 0)
                        {
                            map[tok[..eq]] = tok[(eq + 1)..];
                        }
                    }
                    if (map.Count > 0)
                    {
                        map["_source"] = Path.GetFileName(f);
                        return map;
                    }
                }
                catch (IOException)
                {
                    // a locked log is skipped, not fatal
                }
            }
        }
        return null;
    }
}
