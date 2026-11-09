// GeneralsX @feature wh1ter0se 20/08/2026 The build-hotkey bindings, as the ENGINE reads them.
//
// WHY THIS FILE EXISTS AT ALL
//
// Until 20/08/2026 the letter on a build cameo was painted into the artwork. That is why the
// community indicator packs shipped duplicate copies of the same cameo - one per letter - and why
// those letters started lying the moment anybody rebound a key: a picture cannot follow a setting.
//
// The engine now draws the letter itself and resolves it through ONE accessor
// (HotKeyManager::getCommandButtonHotKey), whose precedence is:
//
//     this file  >  the '&' marker inside the localized CONTROLBAR string
//
// So this file is the only thing that can rebind a build key, and the launcher is the only UI for
// it. The game has no screen for this - which is exactly what the design page says.
//
// FORMAT, AND THE TRAP IN IT
//
// It is the engine's standard UserPreferences map: one "key = value" per line, keys stored
// VERBATIM and compared case-sensitively (UserPreferences.cpp reads them with no case folding).
// The key is the CommandButton's INI name exactly as CommandButton.ini spells it:
//
//     Command_ConstructChinaDozer = J
//     Early_Command_LeafletDrop   = L
//
// The trap is that those names ALREADY begin with "Command_", and a good number begin with
// "Early_Command_". An earlier draft of the engine side required a prefix of its own, which would
// have meant writing "Command_Command_ConstructChinaDozer" and would have left every Early_ button
// unbindable. Do not add a prefix here.
//
// ShowHotKeyIndicators is the one reserved key that is NOT a button name; the engine skips it when
// building the override map.
//
// NOTHING HERE TOUCHES dataID. This file lives in the game's USER data folder, not in a BIG
// archive, and dataID's loose half hashes only CommandButton.ini / CommandSet.ini (SimulationId.cpp
// SIMID_LOOSE_* allow-list). Measured: rebinding a key does not change who you can play with. That
// property is the whole reason the bindings live here rather than in the game data.
using System;
using System.Collections.Generic;
using System.IO;

namespace GeneralsX.Loader;

internal sealed class Keybinds
{
    /// <summary>The reserved key that is not a CommandButton name.</summary>
    public const string ShowIndicatorsKey = "ShowHotKeyIndicators";

    /// <summary>Draw the letter over the cameo at all. Engine default is on.</summary>
    public bool ShowIndicators { get; set; } = true;

    /// <summary>CommandButton name -> single character. Ordinal comparison: the engine's map is
    /// keyed by AsciiString and does not fold case.</summary>
    public Dictionary<string, string> Map { get; } = new(StringComparer.Ordinal);

    // GeneralsX @bugfix Claude 23/09/2026 Was a fourth verbatim copy of the user-data folder. The
    // engine reads this file from getPath_UserData(), which takes its leaf from the registry, so a
    // literal here is a file the game never opens. OwnedPaths.UserDataDir is the one resolver.
    public static string PathOf() =>
        Path.Combine(OwnedPaths.UserDataDir, "CommandButtonHotKeys.ini");

    public static Keybinds Load()
    {
        var k = new Keybinds();
        try
        {
            var p = PathOf();
            if (!File.Exists(p))
            {
                return k;   // no file is the normal case: the CSF '&' markers stay in charge
            }
            foreach (var raw in File.ReadAllLines(p))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith('#') || line.StartsWith(';'))
                {
                    continue;
                }
                var eq = line.IndexOf('=');
                if (eq <= 0)
                {
                    continue;
                }
                var key = line[..eq].Trim();
                var val = line[(eq + 1)..].Trim();
                if (key.Length == 0 || val.Length == 0)
                {
                    continue;
                }
                if (string.Equals(key, ShowIndicatorsKey, StringComparison.OrdinalIgnoreCase))
                {
                    // UserPreferences::getBool treats "yes"/"1"/"true" as true.
                    k.ShowIndicators = val is "1" or "yes" or "Yes" or "YES" or "true" or "True";
                    continue;
                }
                // Only single characters. The engine refuses anything longer, because a longer
                // binding can never be matched by executeHotKey and would leave a letter drawn on
                // a button that does nothing.
                if (val.Length != 1)
                {
                    continue;
                }
                k.Map[key] = val;
            }
        }
        catch (IOException)
        {
            // An unreadable file means "no overrides", never a crash on the settings screen.
        }
        return k;
    }

    /// <summary>
    /// Write the file. Returns false if it could not be written, so the caller can say so rather
    /// than reporting a save that did not happen.
    ///
    /// Written whole, not merged: this launcher is the only writer, and a merge would keep
    /// resurrecting bindings the player has just cleared.
    /// </summary>
    public bool Save()
    {
        try
        {
            var p = PathOf();
            var dir = Path.GetDirectoryName(p);
            if (dir is not null)
            {
                Directory.CreateDirectory(dir);
            }
            var lines = new List<string>
            {
                "; GeneralsX build hotkeys. Written by the launcher's Keybinds page.",
                "; One line per command button, keyed by its CommandButton.ini name.",
                ShowIndicatorsKey + " = " + (ShowIndicators ? "yes" : "no"),
            };
            foreach (var kv in Map)
            {
                if (kv.Value.Length == 1)
                {
                    lines.Add(kv.Key + " = " + kv.Value);
                }
            }
            File.WriteAllLines(p, lines);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// Every key bound more than once within the same set of buttons.
    ///
    /// This matters more than it looks. The engine's addHotKey REFUSES a key another visible button
    /// already owns, and since 20/08/2026 it draws no letter at all on the button that lost. So a
    /// clash does not merely confuse - it silently removes one of the two indicators. Surfacing the
    /// count here is what lets the page say "1 clash" instead of the player finding out in a match.
    ///
    /// Case-insensitive, because the engine lowercases both sides before comparing.
    /// </summary>
    public static HashSet<string> Clashes(IEnumerable<string> keysInScope)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var dupes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var k in keysInScope)
        {
            if (string.IsNullOrEmpty(k))
            {
                continue;
            }
            if (!seen.Add(k))
            {
                dupes.Add(k);
            }
        }
        return dupes;
    }
}
