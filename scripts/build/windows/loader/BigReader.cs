// GeneralsX @feature wh1ter0se 20/08/2026 Read one file out of the mounted .big archives.
//
// WHY THE LAUNCHER SUDDENLY NEEDS THIS
//
// The Keybinds page has to list what each structure actually builds, and that lives in
// Data\INI\CommandSet.ini and Data\INI\CommandButton.ini - neither of which exists as a loose file
// on disk. They are inside the archives. Nothing in the launcher had ever needed to look inside
// one before.
//
// FORMAT (the size field's endianness really is inconsistent; that is the format, not a bug):
//
//   'BIGF' | uint32 LE total size | uint32 BE entry count | uint32 BE first data offset
//   per entry: uint32 BE offset, uint32 BE size, NUL-terminated name
//   then the file data
//
// LOAD ORDER IS THE WHOLE GAME HERE, AND GETTING IT BACKWARDS IS SILENT.
//
// The engine mounts every *.big in the folder, walking them in case-insensitive sorted order
// (FilenameList is a std::set<AsciiString, less_than_nocase>), and loadIntoDirectoryTree is called
// with overwrite=FALSE, which APPENDS - so lookup takes the FIRST archive that had the name. In
// other words the alphabetically-first archive wins, which is why every community override is
// prefixed with '!' or a number: '!' is 0x21 and sorts before every letter and digit.
//
// That matters concretely: the community patch ships a deliberate ZERO-BYTE CommandButton.ini so a
// mod can beat it, and '5' sorts before 'I' of INIZH.big. Read them in the wrong order and you get
// an empty file and conclude the game has no command buttons.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace GeneralsX.Loader;

internal static class BigReader
{
    /// <summary>
    /// The first archive, in the engine's own mount order, that carries <paramref name="entry"/>
    /// with a non-empty body. Null when nothing has it.
    ///
    /// Non-empty is part of the contract on purpose: a zero-byte entry is a real technique in this
    /// data set ("I am deliberately not providing this, let someone else win"), and treating it as
    /// a hit would make the caller parse nothing and report an empty game.
    /// </summary>
    public static byte[]? Read(IEnumerable<string> archiveDirs, string entry)
    {
        foreach (var big in MountOrder(archiveDirs))
        {
            try
            {
                var bytes = ReadFrom(big, entry);
                if (bytes is { Length: > 0 })
                {
                    return bytes;
                }
            }
            catch (IOException)
            {
                // A locked or truncated archive is skipped, not fatal: the next one may have it.
            }
        }
        return null;
    }

    /// <summary>
    /// Every *.big across the given directories, ordered the way the engine mounts them.
    ///
    /// Directories are honoured in the order given, because that is the order the engine scans
    /// them in (primary asset dir, then the patch overlay); WITHIN a directory the sort is the
    /// engine's case-insensitive one.
    /// </summary>
    public static List<string> MountOrder(IEnumerable<string> dirs)
    {
        var all = new List<string>();
        foreach (var d in dirs)
        {
            if (string.IsNullOrEmpty(d) || !Directory.Exists(d))
            {
                continue;
            }
            var here = new List<string>(Directory.EnumerateFiles(d, "*.big", SearchOption.TopDirectoryOnly));
            here.Sort((a, b) => string.Compare(
                Path.GetFileName(a), Path.GetFileName(b), StringComparison.OrdinalIgnoreCase));
            all.AddRange(here);
        }
        return all;
    }

    private static byte[]? ReadFrom(string bigPath, string entry)
    {
        using var fs = new FileStream(bigPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var br = new BinaryReader(fs);

        if (fs.Length < 16)
        {
            return null;
        }
        var magic = br.ReadBytes(4);
        if (magic.Length != 4 || magic[0] != (byte)'B' || magic[1] != (byte)'I' ||
            magic[2] != (byte)'G' || magic[3] != (byte)'F')
        {
            return null;
        }
        _ = br.ReadUInt32();                 // total size, little-endian, unused
        var count = BE(br.ReadUInt32());
        _ = BE(br.ReadUInt32());             // first data offset, unused

        // A corrupt count would otherwise drive a multi-million iteration loop over a small file.
        if (count == 0 || count > 200000)
        {
            return null;
        }

        var want = Normalise(entry);
        for (uint i = 0; i < count; i++)
        {
            var off = BE(br.ReadUInt32());
            var len = BE(br.ReadUInt32());
            var name = ReadCString(br);
            if (name is null)
            {
                return null;
            }
            if (!Normalise(name).Equals(want, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            if (off > fs.Length || off + (long)len > fs.Length)
            {
                return null;         // truncated archive: the entry claims bytes that are not there
            }
            fs.Seek(off, SeekOrigin.Begin);
            return br.ReadBytes((int)len);
        }
        return null;
    }

    /// <summary>Archive names use backslashes; callers should not have to care which way they typed it.</summary>
    private static string Normalise(string s) => s.Replace('/', '\\').TrimStart('\\');

    private static uint BE(uint v) =>
        ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);

    private static string? ReadCString(BinaryReader br)
    {
        var sb = new StringBuilder(64);
        for (var guard = 0; guard < 1024; guard++)
        {
            int b;
            try
            {
                b = br.ReadByte();
            }
            catch (EndOfStreamException)
            {
                return null;
            }
            if (b == 0)
            {
                return sb.ToString();
            }
            sb.Append((char)b);
        }
        return null;    // no terminator inside a sane length: treat the table as unreadable
    }
}
