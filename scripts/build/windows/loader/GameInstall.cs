/* Finding the player's existing Zero Hour, and deciding where GeneralsX may write.
 *
 * GeneralsX @feature Claude 08/08/2026
 *
 * EVERY PATH IN THIS FILE IS EITHER READ-ONLY OR OURS. That split is the promise the consent
 * screen makes, so it is expressed here as types rather than as discipline: GameData carries a
 * directory this program only ever reads, and OwnedPaths carries the three places it may write.
 * If a future edit wants to write somewhere else it has to add it to OwnedPaths, which is also the
 * list the consent screen prints and the uninstaller removes. One list, three consumers, no drift.
 */

using Microsoft.Win32;

namespace GeneralsX.Loader;

/// <summary>A Zero Hour installation we found. Read-only, always.</summary>
internal sealed record GameData(string Path, string Source)
{
    /// <summary>
    /// The marker that proves a folder is ZERO HOUR and not plain Generals.
    ///
    /// This used to be { "INI.big", "W3D.big" } and that was wrong in both directions: Zero Hour
    /// ships INIZH.big / W3DZH.big, so a real Zero Hour install was REJECTED, while a base Generals
    /// folder - which does contain INI.big - would have been accepted and then failed much later
    /// with a data mismatch nobody could diagnose. Verified against a live Steam install: the
    /// folder contains INIZH.big, AudioZH.big, MapsZH.big, EnglishZH.big and no INI.big at all.
    ///
    /// INIZH.big alone is decisive, so it is the only hard requirement; the expansion cannot be
    /// installed without it and the base game never has it.
    /// </summary>
    private static readonly string[] Required = { "INIZH.big" };

    public static bool LooksLikeZeroHour(string? dir)
    {
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir))
        {
            return false;
        }
        // Case-insensitive on Windows, but enumerate rather than probe: some installs ship
        // lower-case names and File.Exists would be fine, while a Steam depot repair can leave a
        // zero-byte placeholder that exists and is useless.
        var names = new HashSet<string>(
            Directory.EnumerateFiles(dir).Select(System.IO.Path.GetFileName)!,
            StringComparer.OrdinalIgnoreCase);
        return Required.All(names.Contains);
    }

    /// <summary>
    /// Everywhere Zero Hour is normally installed, best guess first. Registry before disk, because
    /// a registry hit is the install the user actually launches; a folder on disk may be a copy.
    /// </summary>
    public static IEnumerable<GameData> Candidates()
    {
        foreach (var (key, name, source) in new[]
                 {
                     (@"SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour",
                         "InstallPath", "EA / retail"),
                     (@"SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour",
                         "InstallPath", "EA / retail"),
                     (@"SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Generals Zero Hour",
                         "InstallPath", "EA / retail"),
                 })
        {
            var p = ReadRegistry(key, name);
            if (LooksLikeZeroHour(p))
            {
                // The EA key frequently points at a Steam install. Naming the record rather than
                // the location made the consent screen say "EA / retail" above a steamapps path,
                // which reads as though we found the wrong thing.
                yield return new GameData(p!, DescribeLocation(p!, source));
            }
        }

        // The EA App and Origin do not always leave the old EA Games registry keys behind, and a
        // reinstall through a different launcher can leave them pointing at a folder that no longer
        // has the game in it. Registry first because a hit there is the install the user actually
        // launches - but never registry ONLY, which is where this used to stop.
        foreach (var p in InstallDirCandidates())
        {
            if (LooksLikeZeroHour(p))
            {
                yield return new GameData(p, "EA / Origin");
            }
        }

        foreach (var lib in SteamLibraries())
        {
            // Steam has shipped this under two spellings; check both rather than guessing.
            foreach (var leaf in GameFolderNames)
            {
                var p = System.IO.Path.Combine(lib, "steamapps", "common", leaf);
                if (LooksLikeZeroHour(p))
                {
                    yield return new GameData(p, "Steam");
                }
            }
        }

        // Last resort: whatever the uninstall entries say. This catches launchers we have never
        // heard of, and installs the user moved by hand, because Windows records the location for
        // anything that can be uninstalled from Settings.
        foreach (var p in UninstallEntryPaths())
        {
            if (LooksLikeZeroHour(p))
            {
                yield return new GameData(p, "installed programs");
            }
        }
    }

    /// <summary>
    /// Every folder name this game has shipped under. The ampersand form, the spelled-out form, and
    /// the retail disc's "Zero Hour" alone - all three exist in the wild.
    /// </summary>
    private static readonly string[] GameFolderNames =
    {
        "Command & Conquer Generals - Zero Hour",
        "Command and Conquer Generals Zero Hour",
        "Command & Conquer Generals Zero Hour",
        "Command and Conquer Generals - Zero Hour",
        "Zero Hour",
    };

    /// <summary>
    /// The launcher folders EA has used across twenty years, on every fixed drive rather than just
    /// C:. People move game libraries to a second drive constantly and the old code would simply
    /// not find those.
    /// </summary>
    private static IEnumerable<string> InstallDirCandidates()
    {
        var parents = new[]
        {
            @"Program Files (x86)\Origin Games",
            @"Program Files\Origin Games",
            @"Program Files (x86)\EA Games",
            @"Program Files\EA Games",
            @"Program Files (x86)\EA GAMES",          // the retail disc used caps
            @"Program Files\EA GAMES",
            @"Program Files (x86)\Electronic Arts",
            @"Program Files\Electronic Arts",
            @"EA Games",
            @"Games",
        };

        foreach (var drive in FixedDrives())
        {
            foreach (var parent in parents)
            {
                foreach (var leaf in GameFolderNames)
                {
                    string p;
                    try
                    {
                        p = System.IO.Path.Combine(drive, parent, leaf);
                    }
                    catch
                    {
                        continue;
                    }
                    yield return p;
                }
            }
        }
    }

    private static IEnumerable<string> FixedDrives()
    {
        // Collected before the loop rather than inside a try/catch around it: C# forbids `yield`
        // in a catch clause, and enumerating drives is exactly the call that throws on a machine
        // with a disconnected network drive or an empty card reader.
        DriveInfo[] drives;
        try
        {
            drives = DriveInfo.GetDrives();
        }
        catch
        {
            drives = Array.Empty<DriveInfo>();
        }

        if (drives.Length == 0)
        {
            yield return @"C:\";
            yield break;
        }

        foreach (var d in drives)
        {
            bool ok;
            try
            {
                // IsReady is the guard: a card reader with no card throws on RootDirectory access.
                ok = d.IsReady && (d.DriveType == DriveType.Fixed || d.DriveType == DriveType.Removable);
            }
            catch
            {
                ok = false;
            }
            if (ok)
            {
                yield return d.RootDirectory.FullName;
            }
        }
    }

    /// <summary>Uninstall entries whose display name looks like Zero Hour.</summary>
    private static IEnumerable<string> UninstallEntryPaths()
    {
        var roots = new[]
        {
            @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
            @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
        };

        foreach (var root in roots)
        {
            string[] subs;
            RegistryKey? k = null;
            try
            {
                k = Registry.LocalMachine.OpenSubKey(root);
                subs = k?.GetSubKeyNames() ?? Array.Empty<string>();
            }
            catch
            {
                continue;
            }

            foreach (var sub in subs)
            {
                string? name = null, loc = null;
                try
                {
                    using var e = k!.OpenSubKey(sub);
                    name = e?.GetValue("DisplayName") as string;
                    loc = e?.GetValue("InstallLocation") as string;
                }
                catch
                {
                    continue;
                }

                if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(loc))
                {
                    continue;
                }
                if (name.IndexOf("Zero Hour", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }
                yield return loc;
            }
            k?.Dispose();
        }
    }

    /// <summary>Describe where the folder IS, falling back to how we found it.</summary>
    private static string DescribeLocation(string path, string fallback)
    {
        if (path.IndexOf(@"\steamapps\", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            return "Steam";
        }
        if (path.IndexOf(@"\Origin Games\", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            return "Origin";
        }
        return fallback;
    }

    private static string? ReadRegistry(string key, string name)
    {
        try
        {
            using var k = Registry.LocalMachine.OpenSubKey(key);
            return k?.GetValue(name) as string;
        }
        catch
        {
            return null;    // a locked-down machine is not an error, it is just no answer
        }
    }

    /// <summary>
    /// Steam libraries can live on any drive, so read libraryfolders.vdf rather than guessing.
    /// A stale entry pointing at a removed drive is normal and must not throw - the PowerShell
    /// installer learned that one the hard way (DriveNotFoundException, not a null).
    /// </summary>
    private static IEnumerable<string> SteamLibraries()
    {
        var roots = new List<string>();
        var steam = ReadRegistry(@"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")
                    ?? @"C:\Program Files (x86)\Steam";
        roots.Add(steam);

        var vdf = System.IO.Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        try
        {
            if (File.Exists(vdf))
            {
                foreach (var line in File.ReadLines(vdf))
                {
                    // "path"  "D:\\SteamLibrary"
                    var i = line.IndexOf("\"path\"", StringComparison.OrdinalIgnoreCase);
                    if (i < 0)
                    {
                        continue;
                    }
                    var parts = line.Split('"', StringSplitOptions.RemoveEmptyEntries);
                    var last = parts.LastOrDefault()?.Trim();
                    if (!string.IsNullOrWhiteSpace(last))
                    {
                        roots.Add(last.Replace(@"\\", @"\"));
                    }
                }
            }
        }
        catch
        {
            // Unreadable library list -> fall back to the default location only.
        }

        foreach (var r in roots.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            bool ok;
            try
            {
                ok = Directory.Exists(r);
            }
            catch
            {
                ok = false;     // removed drive
            }
            if (ok)
            {
                yield return r;
            }
        }
    }
}

/// <summary>
/// The complete list of places this program writes. The consent screen prints exactly this, and
/// the uninstaller removes exactly this. Nothing else may be written anywhere.
/// </summary>
internal static class OwnedPaths
{
    /// <summary>
    /// A TEST SEAM, and the reason it is in shipping code rather than in a harness.
    ///
    /// Every path below resolves through Environment.GetFolderPath, which goes to the Windows
    /// known-folder API and cannot be redirected by setting %LOCALAPPDATA% on a child process. So
    /// there is no way to run the installer for real - download, unpack, shortcuts, uninstaller -
    /// without it writing into the operator's actual install folder and putting real shortcuts on
    /// their actual desktop. The choice is a four-line seam or an installer whose main path is
    /// never executed before a stranger runs it.
    ///
    /// It redirects only where this program writes ITS OWN files. It grants nothing: anybody who
    /// can set your environment can already run anything as you.
    /// </summary>
    private static string? TestRoot =>
        Environment.GetEnvironmentVariable("GX_TEST_ROOT") is { Length: > 0 } r ? r : null;

    private static string Folder(Environment.SpecialFolder f, string leaf) =>
        TestRoot is null
            ? Environment.GetFolderPath(f)
            : System.IO.Path.Combine(TestRoot, leaf);

    /// <summary>
    /// What an engine packaged before 23/09/2026 falls back to when the registry has no
    /// UserDataLeafName. The DEFAULT, not the answer, and only for such an engine - see
    /// <see cref="LegacyUserDataDir"/>.
    /// </summary>
    internal const string UserDataLeafDefault = "Command and Conquer Generals Zero Hour Data";

    /// <summary>
    /// The BUILD.txt "user data" value of an engine that keeps its own folder. package-windows-share.ps1
    /// writes exactly this; scripts/launcher-rs/gxio/src/release.rs guards that the two agree, because
    /// a mismatch leaves this launcher on the legacy folder beside the new engine - the "resolution is
    /// never applied" bug again.
    /// </summary>
    internal const string OwnUserDataStamp = @"Documents\GeneralsX\GeneralsZH";

    /// <summary>
    /// THE ENGINE'S USER-DATA FOLDER, RESOLVED THE WAY THE INSTALLED ENGINE RESOLVES IT. Every file
    /// below that the GAME reads is built from this, so the two cannot disagree.
    ///
    /// GeneralsX @feature Claude 23/09/2026 An engine packaged from 23/09/2026 keeps GeneralsX's OWN
    /// folder, Documents\GeneralsX\GeneralsZH (GlobalData.cpp, owner decision). Older ones use the
    /// folder retail Zero Hour and Generals Online share, which <see cref="LegacyUserDataDir"/>
    /// resolves. This launcher updates on its own Velopack feed, so it can sit beside either. The
    /// installed engine's BUILD.txt says which it is (<see cref="OwnUserDataStamp"/>), and a
    /// resolution chosen here therefore lands in the folder the installed engine actually reads. A
    /// BUILD.txt that is missing or unreadable means an old or developer install, so the legacy folder.
    /// The new engine copies the old folder's settings across once, on its first start.
    ///
    /// GeneralsX @bugfix Claude 23/09/2026 AND NOT BEFORE THAT FIRST START. Until the engine has run its
    /// one-time migration - GXMigration.txt in the new folder, GX_MIGRATION_MARKER in
    /// GXUserDataMigrate.h, written by every pass that got as far as looking - this stays on the
    /// legacy folder. Otherwise a camera or HUD change made here first read the EMPTY new folder, filled
    /// the value the player did not touch from a default (both are written together, OptionsForm.cs's
    /// SAVE and HudPage.cs), and the migration's never-replace-a-key merge then kept that default over
    /// the player's old value (review of the merged folder move, finding 3). Writing to the legacy
    /// folder until then is what every launcher did before 23/09/2026, and the migration brings it all.
    /// ponytail: a marker from a PARTIAL pass counts, so a settings file that pass could not read is
    /// merged again later with this launcher's keys first. A per-file "settled:" test if that matters.
    ///
    /// The paragraphs below are the history of the LEGACY arm and still describe it exactly.
    ///
    /// GeneralsX @bugfix Claude 23/09/2026 Options.ini, SagePatch.ini, GXHud.ini,
    /// CommandButtonHotKeys.ini and the About page each pasted the fallback leaf name in
    /// literally, so on any machine whose registry names a different leaf this program read and
    /// wrote a folder the game never opens. The player-visible failure is precise and was
    /// reported: set a resolution here, press save, and the game still starts at 800x600 -
    /// because OptionPreferences::getResolution (OptionPreferences.cpp:929-942) finds no
    /// "Resolution" key in the file the ENGINE opened and returns TheGlobalData's
    /// DEFAULT_DISPLAY_WIDTH/HEIGHT (GameDefines.h:312-313), which OptionsMenu.cpp:259 calls "our
    /// lowest supported mode". Changing it from the in-game options screen sticks because that
    /// screen writes through getPath_UserData() - the engine's own resolver, both directions.
    ///
    /// The comment on GxHudIni used to record this divergence as a known, accepted property of all
    /// three paths. It is fixed rather than recorded now, and 04d61e0c2 did the same thing for the
    /// Rust launcher's five call sites - leaving this program as the last copy of the mistake, and
    /// the only one that still disagreed with the other launcher on the same machine.
    ///
    /// HKCU BEFORE HKLM, and an EMPTY value is an answer rather than an absence, because that is
    /// what the engine does: GetStringFromRegistry (registry.cpp:411-427, the "#else // Windows
    /// implementation" arm - the _UNIX one at :271 with its CNC_ZH_ environment override is not
    /// compiled here) tries HKEY_CURRENT_USER then HKEY_LOCAL_MACHINE, and GlobalData.cpp:1538-1547
    /// concatenates whatever comes back. Matching the engine is the whole job; being more correct
    /// than it would reintroduce the same disagreement from the other side.
    /// </summary>
    internal static string UserDataDir =>
        string.Equals(BuildInfo.Read(InstallDir)?.UserData, OwnUserDataStamp, StringComparison.OrdinalIgnoreCase)
            && System.IO.File.Exists(System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "GXMigration.txt"))
            ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH")
            : LegacyUserDataDir;

    /// <summary>
    /// The folder an engine packaged before 23/09/2026 uses: Documents + the registry's
    /// UserDataLeafName, else <see cref="UserDataLeafDefault"/>. Only <see cref="UserDataDir"/> may
    /// read this. A call site that did would write to the shared folder whichever engine is installed.
    /// </summary>
    internal static string LegacyUserDataDir => System.IO.Path.Combine(
        Folder(Environment.SpecialFolder.MyDocuments, "documents"),
        ReadUserDataLeaf() ?? UserDataLeafDefault);

    private static string? ReadUserDataLeaf()
    {
        const string sub = @"SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour";
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
        {
            try
            {
                using var k = root.OpenSubKey(sub);
                if (k?.GetValue("UserDataLeafName") is string s)
                {
                    return s;
                }
            }
            catch
            {
                // a locked-down machine is not an error, it is just no answer - same as ReadRegistry
            }
        }
        return null;
    }

    /// <summary>
    /// WHERE THIS LAUNCHER ACTUALLY LIVES - not where we wish it lived.
    ///
    /// GeneralsX @bugfix wh1ter0se 12/08/2026 This used to be the hardcoded
    /// %LOCALAPPDATA%\Programs\GeneralsX regardless of where setup had put us, and every one of the
    /// nine call sites inherited that lie. A user who picked a custom folder got a COMPLETE, working
    /// install that the launcher could not see: GameInstalled() checked the hardcoded path, found no
    /// generalszh.exe, and reported "Could not install the game. Check your connection and try
    /// again." - blaming the network for a directory mistake.
    ///
    /// Measured on a real user: install at C:\Games\GeneralsX with generalszh.exe (7,687 KB) and
    /// every DLL present, while %LOCALAPPDATA%\Programs\GeneralsX held exactly ONE file -
    /// gxlauncher.ini, written there by PathOf() below. That stray settings file is the fingerprint
    /// of this bug, and it also means the same user got two different settings depending on which
    /// copy of the launcher they clicked.
    ///
    /// AppContext.BaseDirectory is the directory of the running executable, so the launcher now
    /// agrees with itself by construction. The old constant remains ONLY as a fallback for the case
    /// where that cannot be resolved, which should never happen for a normal process but is not
    /// worth crashing over.
    ///
    /// GX_TEST_ROOT still wins, because the tests depend on redirecting this.
    /// </summary>
    public static string InstallDir
    {
        get
        {
            if (TestRoot is not null)
            {
                return System.IO.Path.Combine(TestRoot, "local", "Programs", "GeneralsX");
            }

            try
            {
                var here = AppContext.BaseDirectory;
                if (!string.IsNullOrEmpty(here))
                {
                    return System.IO.Path.TrimEndingDirectorySeparator(here);
                }
            }
            catch
            {
                // Fall through to the historical default rather than fail: an unresolvable base
                // directory is not a reason to refuse to launch.
            }

            return System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Programs", "GeneralsX");
        }
    }

    /// <summary>Start Menu and Desktop shortcuts.</summary>
    public static string StartMenuLink => System.IO.Path.Combine(
        Folder(Environment.SpecialFolder.ApplicationData, "roaming"),
        @"Microsoft\Windows\Start Menu\Programs", "GeneralsX.lnk");

    public static string DesktopLink => System.IO.Path.Combine(
        Folder(Environment.SpecialFolder.Desktop, "desktop"), "GeneralsX.lnk");

    /// <summary>
    /// THE ONE FILE OUTSIDE OUR OWN FOLDER. Options.ini belongs to the game, and under an engine that
    /// predates 23/09/2026 it is shared with retail Zero Hour AND with the Generals Online client
    /// (see <see cref="UserDataDir"/>), so this program sets three keys in it and nothing else,
    /// leaving every other preference alone. The consent screen says this in plain
    /// words rather than burying it, because a friend who later finds an edited file they were not
    /// told about has every reason to distrust the next thing we ship.
    ///
    /// RelayAddress and RelayRoom are ours: we invented them, nothing else writes them, and the
    /// uninstaller strips them. Resolution is the third, and it is DIFFERENT - it is one of the
    /// game's own long-standing keys, which the options screen edits because that is the only place
    /// the engine reads a resolution from (OptionPreferences::getResolution). We change its value;
    /// we do not own it.
    /// </summary>
    public static string OptionsIni => System.IO.Path.Combine(UserDataDir, "Options.ini");

    /// <summary>
    /// SagePatch.ini, beside Options.ini in the same user-data folder. UNLIKE Options.ini this file is
    /// OURS: the engine loads it as a GameData override but restricts the load to six client-side
    /// camera/render fields, so nothing written here can reach the shared simulation. The engine writes
    /// a default copy only when the file is absent; the launcher owns the camera block once it writes.
    /// </summary>
    public static string SagePatchIni => System.IO.Path.Combine(UserDataDir, "SagePatch.ini");

    /// <summary>
    /// SagePatch.ini where the INSTALLED ENGINE reads it on its next start: <see cref="SagePatchIni"/>,
    /// except that an engine keeping its own folder reads the new one even before its migration has run.
    /// GameSagePatch.EnsureDefaults only (review of 2927d052a, finding 1): its untouched defaults written
    /// into the legacy folder are dropped by the migration (kUntouchedSagePatch), and the engine then
    /// writes its own MaxCameraHeight instead (350 before 25/09/2026, this launcher's fixed 310
    /// since). In the new folder the migration keeps them, or replaces them whole with an old file
    /// that holds a player's choice.
    /// </summary>
    public static string SagePatchIniForDefaults =>
        string.Equals(BuildInfo.Read(InstallDir)?.UserData, OwnUserDataStamp, StringComparison.OrdinalIgnoreCase)
            ? System.IO.Path.Combine(Folder(Environment.SpecialFolder.MyDocuments, "documents"), "GeneralsX", "GeneralsZH", "SagePatch.ini")
            : SagePatchIni;

    /// <summary>
    /// GXHud.ini, the third file in that same folder, and the most clearly OURS of the three.
    ///
    /// The engine builds this path as getPath_UserData() + "GXHud.ini" (InGameUI.cpp:1012-1013).
    /// getPath_UserData() is Documents\GeneralsX\GeneralsZH for an engine packaged from 23/09/2026. An
    /// older engine uses Documents + the registry's UserDataLeafName, falling back to the literal
    /// "Command and Conquer Generals Zero Hour Data". <see cref="UserDataDir"/> follows whichever
    /// engine is installed.
    ///
    /// GeneralsX @bugfix Claude 23/09/2026 This paragraph used to end "the fallback is what is
    /// reproduced here ... a machine whose registry names a different leaf would have the launcher
    /// and the engine looking in two different folders ... it is recorded here rather than
    /// discovered later." It was discovered later anyway, as a player on Windows whose launcher
    /// resolution never reached the game. All three paths now derive from <see cref="UserDataDir"/>,
    /// which makes the same two lookups the engine makes.
    ///
    /// NOTHING WRITES IT BUT US. gxHudSetting opens it "r" and there is no other reference to the
    /// name anywhere in the engine, so unlike Options.ini there is no in-memory map waiting to be
    /// dumped back over our write when the game exits. That is why the anchor is saved even while
    /// Zero Hour is running - see SaveHud in HudPage.cs.
    /// </summary>
    public static string GxHudIni => System.IO.Path.Combine(UserDataDir, "GXHud.ini");
}
