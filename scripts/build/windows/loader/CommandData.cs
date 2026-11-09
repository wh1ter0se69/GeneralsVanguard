// GeneralsX @feature wh1ter0se 20/08/2026 What each structure builds, and what letter it shows.
//
// Three sources, all read out of the archives (BigReader), all of them the SAME bytes the engine
// will use - which is the only way this page can promise the letter it draws is the letter you get:
//
//   Data\INI\CommandSet.ini    structure -> the buttons in its command bar, by slot
//   Data\INI\CommandButton.ini button -> its TextLabel (a CSF key) and what it does
//   Data\<lang>\generals.csf    that CSF key -> the localized string, whose '&' marks the letter
//
// THE '&' IS THE DEFAULT, NOT THE ANSWER. HotKeyManager::getCommandButtonHotKey consults the
// user's CommandButtonHotKeys.ini FIRST and only falls back to the '&'. So the letter shown for a
// button is: override if present, else the '&' character, else nothing.
//
// WHY GROUPING IS BY COMMANDSET NAME AND NOT BY OBJECT
//
// The authoritative structure -> command set link lives on each object in Data\INI\Object\*.ini,
// which is hundreds of files. The command set NAMES already encode both facts the page groups by
// ("ChinaBarracksCommandSet", "GLAArmsDealerCommandSet"), so this reads them instead. That is a
// heuristic and it is written down as one: a set whose name does not start with a faction is
// listed under Other rather than being silently dropped, because a build button nobody can find is
// worse than an untidy heading.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace GeneralsX.Loader;

internal sealed record BuildButton(string Command, string Label, string DefaultKey);

internal sealed record BuildSet(string Faction, string Structure, string SetName, List<BuildButton> Buttons);

internal static class CommandData
{
    /// <summary>
    /// Everything the Keybinds page lists. Empty when the archives could not be read, which the
    /// page reports rather than rendering an empty tree that looks like "your game has no units".
    /// </summary>
    public static List<BuildSet> Load(IEnumerable<string> archiveDirs)
    {
        var dirs = new List<string>(archiveDirs);
        var sets = new List<BuildSet>();

        var csBytes = BigReader.Read(dirs, @"Data\INI\CommandSet.ini");
        var cbBytes = BigReader.Read(dirs, @"Data\INI\CommandButton.ini");
        if (csBytes is null || cbBytes is null)
        {
            return sets;
        }

        var labels = ParseButtonLabels(Encoding.ASCII.GetString(cbBytes));
        var csf = LoadCsf(dirs);

        foreach (var (setName, commands) in ParseCommandSets(Encoding.ASCII.GetString(csBytes)))
        {
            var buttons = new List<BuildButton>();
            foreach (var cmd in commands)
            {
                labels.TryGetValue(cmd, out var labelKey);
                var text = labelKey is not null && csf.TryGetValue(labelKey, out var s) ? s : null;
                buttons.Add(new BuildButton(cmd, Pretty(cmd, text), Ampersand(text)));
            }
            if (buttons.Count == 0)
            {
                continue;
            }
            var (faction, structure) = Split(setName);
            sets.Add(new BuildSet(faction, structure, setName, buttons));
        }
        sets.Sort((a, b) =>
        {
            var f = string.Compare(a.Faction, b.Faction, StringComparison.OrdinalIgnoreCase);
            return f != 0 ? f : string.Compare(a.Structure, b.Structure, StringComparison.OrdinalIgnoreCase);
        });
        return sets;
    }

    /// <summary>CommandSet blocks: a name, then numbered slots naming command buttons.</summary>
    private static IEnumerable<(string, List<string>)> ParseCommandSets(string text)
    {
        string? name = null;
        var cmds = new List<string>();
        foreach (var raw in text.Split('\n'))
        {
            var line = Strip(raw);
            if (line.Length == 0)
            {
                continue;
            }
            if (line.StartsWith("CommandSet ", StringComparison.OrdinalIgnoreCase))
            {
                name = line[11..].Trim();
                cmds = new List<string>();
                continue;
            }
            if (line.Equals("End", StringComparison.OrdinalIgnoreCase))
            {
                if (name is not null && cmds.Count > 0)
                {
                    yield return (name, cmds);
                }
                name = null;
                continue;
            }
            if (name is null)
            {
                continue;
            }
            // Slots are "<n> = Command_Xxx". The slot number is the command bar position and is
            // deliberately not kept: the page lists what the structure builds, and a gap in the
            // numbering is a layout fact, not a binding one.
            var eq = line.IndexOf('=');
            if (eq <= 0)
            {
                continue;
            }
            var v = line[(eq + 1)..].Trim();
            if (v.Length > 0 && !v.Equals("None", StringComparison.OrdinalIgnoreCase))
            {
                cmds.Add(v);
            }
        }
    }

    /// <summary>CommandButton name -> its TextLabel CSF key.</summary>
    private static Dictionary<string, string> ParseButtonLabels(string text)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string? name = null;
        foreach (var raw in text.Split('\n'))
        {
            var line = Strip(raw);
            if (line.StartsWith("CommandButton ", StringComparison.OrdinalIgnoreCase))
            {
                name = line[14..].Trim();
                continue;
            }
            if (line.Equals("End", StringComparison.OrdinalIgnoreCase))
            {
                name = null;
                continue;
            }
            if (name is null || !line.StartsWith("TextLabel", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            var eq = line.IndexOf('=');
            if (eq > 0)
            {
                map[name] = line[(eq + 1)..].Trim();
            }
        }
        return map;
    }

    /// <summary>Everything after a ';' is a comment, and the game's INI is full of them.</summary>
    private static string Strip(string raw)
    {
        var s = raw.Replace('\r', ' ');
        var c = s.IndexOf(';');
        if (c >= 0)
        {
            s = s[..c];
        }
        return s.Trim();
    }

    /// <summary>The character after '&amp;', which is what searchHotKey returns.</summary>
    private static string Ampersand(string? s)
    {
        if (string.IsNullOrEmpty(s))
        {
            return "";
        }
        var i = s.IndexOf('&');
        return i >= 0 && i + 1 < s.Length ? s[i + 1].ToString().ToUpperInvariant() : "";
    }

    /// <summary>A readable name: the localized string without its '&amp;', else the INI name.</summary>
    private static string Pretty(string command, string? text)
    {
        if (!string.IsNullOrEmpty(text))
        {
            var t = text.Replace("&", "").Trim();
            if (t.Length > 0)
            {
                return t;
            }
        }
        var n = command;
        foreach (var p in new[] { "Early_Command_", "Command_" })
        {
            if (n.StartsWith(p, StringComparison.Ordinal))
            {
                n = n[p.Length..];
                break;
            }
        }
        return n;
    }

    private static readonly (string Prefix, string Faction)[] Factions =
    {
        ("America", "USA"), ("USA", "USA"), ("AirF", "USA"), ("Laser", "USA"), ("SuperW", "USA"),
        ("China", "CHINA"), ("Nuke", "CHINA"), ("Tank", "CHINA"), ("Infantry", "CHINA"),
        ("GLA", "GLA"), ("Toxin", "GLA"), ("Stealth", "GLA"), ("Demo", "GLA"), ("Slth", "GLA"),
    };

    private static (string, string) Split(string setName)
    {
        var n = setName;
        if (n.EndsWith("CommandSet", StringComparison.OrdinalIgnoreCase))
        {
            n = n[..^"CommandSet".Length];
        }
        foreach (var (prefix, faction) in Factions)
        {
            if (n.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && n.Length > prefix.Length)
            {
                return (faction, Spaced(n[prefix.Length..]));
            }
        }
        return ("OTHER", Spaced(n));
    }

    /// <summary>"ArmsDealer" -> "Arms Dealer", so the list reads like the design's.</summary>
    private static string Spaced(string s)
    {
        var sb = new StringBuilder(s.Length + 6);
        for (var i = 0; i < s.Length; i++)
        {
            if (i > 0 && char.IsUpper(s[i]) && !char.IsUpper(s[i - 1]))
            {
                sb.Append(' ');
            }
            sb.Append(s[i]);
        }
        return sb.ToString();
    }

    /// <summary>
    /// The string table. CSF stores each string as UTF-16LE with EVERY BIT INVERTED - that is the
    /// format, and reading it without the NOT yields mojibake that looks like an encoding bug.
    /// </summary>
    private static Dictionary<string, string> LoadCsf(List<string> dirs)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        byte[]? bytes = null;
        foreach (var lang in new[] { "English", "German", "French" })
        {
            bytes = BigReader.Read(dirs, $@"Data\{lang}\generals.csf");
            if (bytes is not null)
            {
                break;
            }
        }
        if (bytes is null || bytes.Length < 24)
        {
            return map;
        }
        try
        {
            using var ms = new MemoryStream(bytes);
            using var br = new BinaryReader(ms);
            var magic = new string(br.ReadChars(4));
            if (magic != " FSC")
            {
                return map;
            }
            _ = br.ReadUInt32();                    // version
            var numLabels = br.ReadUInt32();
            _ = br.ReadUInt32();                    // numStrings
            _ = br.ReadUInt32();                    // unused
            _ = br.ReadUInt32();                    // language

            for (uint i = 0; i < numLabels && ms.Position < ms.Length; i++)
            {
                var lbl = new string(br.ReadChars(4));
                if (lbl != " LBL")
                {
                    break;
                }
                var pairs = br.ReadUInt32();
                var nameLen = br.ReadInt32();
                if (nameLen < 0 || nameLen > 4096)
                {
                    break;
                }
                var name = new string(br.ReadChars(nameLen));
                string value = "";
                for (uint p = 0; p < pairs; p++)
                {
                    var kind = new string(br.ReadChars(4));      // " RTS" or "WRTS"
                    var len = br.ReadInt32();
                    if (len < 0 || len > 1 << 20)
                    {
                        return map;
                    }
                    var raw = br.ReadBytes(len * 2);
                    var chars = new char[len];
                    for (var c = 0; c < len; c++)
                    {
                        // bitwise NOT, little-endian pair
                        var lo = (byte)~raw[c * 2];
                        var hi = (byte)~raw[(c * 2) + 1];
                        chars[c] = (char)(lo | (hi << 8));
                    }
                    if (p == 0)
                    {
                        value = new string(chars);
                    }
                    if (kind == "WRTS")
                    {
                        var extra = br.ReadInt32();
                        if (extra is < 0 or > (1 << 20))
                        {
                            return map;
                        }
                        _ = br.ReadBytes(extra);
                    }
                }
                map[name] = value;
            }
        }
        catch (EndOfStreamException)
        {
            // A truncated CSF costs the default letters, not the page.
        }
        return map;
    }
}
