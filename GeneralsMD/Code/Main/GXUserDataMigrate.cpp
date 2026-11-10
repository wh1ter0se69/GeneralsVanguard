// GeneralsX: see GXUserDataMigrate.h for what is moved and the rules.
#include "GXUserDataMigrate.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

// Last: on Windows it brings in <windows.h>, and its macros are kept away from the standard headers.
#include "GameClient/GXReplaceFile.h"

namespace fs = std::filesystem;

namespace
{

// The files brought across ONCE each. The owner's list, 23/09/2026. A settings file added here comes
// across with the same never-overwrite rule.
//
// ONCE means once per file, not once per pass (review of b101653df, P2-1). A file is SETTLED when it
// is copied, merged, found already there, or found absent from the old folder, and the marker
// records it. A settled file is never looked at again, so a pass that stays PARTIAL over one
// unreadable map cannot bring back a GXLogin.txt the player signed out of, or a GXPlayer.ini they
// reset, on every start after. Only the folders below are retried.
//
// MERGE, not copy, for every file a launcher can write in the new folder BEFORE the engine's first
// start:
//   Options.ini               gxlaunch.c (RelayAddress), both launchers' options screens, the harness
//   SagePatch.ini             BOTH launchers at their own startup (Program.cs GameSagePatch.EnsureDefaults,
//                             gxlauncher main.rs sagepatch::ensure_defaults) - always, and whole.
//                             An untouched one, new or old, counts as absent (kUntouchedSagePatch).
//   GXHud.ini                 the HUD page of either launcher, when the player saves it first
//   CommandButtonHotKeys.ini  the Keys page of either launcher, when the player saves it first
// Network.ini and Skirmish*.ini have no writer but the engine, so they are plain copies.
enum How { COPY, MERGE, MERGE_GAMEDATA };
struct Plain { const char *name; How how; };
const char *const kIdentity = "GXPlayer.ini";
const char *const kLogin = "GXLogin.txt";
const Plain kPlainFiles[] = {
	{ kIdentity, COPY }, { kLogin, COPY }, { "Network.ini", COPY },
	{ "Options.ini", MERGE }, { "GXHud.ini", MERGE }, { "CommandButtonHotKeys.ini", MERGE },
	{ "SagePatch.ini", MERGE_GAMEDATA },
};
// Plus every Skirmish*.ini at the top level of the old folder, found in migrate() by isSkirmishIni.
// The folders copied file by file, and retried until every file is across. Save is the campaign and
// skirmish saves, GameState::getSaveDirectory() (GameState.cpp): without it every save is missing
// from the Load menu after the move (review of the merged folder move, finding 1).
const char *const kTrees[] = { "Maps", "Replays", "Save" };
// "settled: Save\" on a finished marker: Save is across. A marker finished before Save joined kTrees
// (integration builds 5c9056b8b to 30c234514, never tagged) lacks it, so Save alone comes across once
// more, same rules (review of 2927d052a, finding 2).
const char *const kSaveSettled = "Save\\";

struct Tally
{
	int copied = 0;
	int present = 0;
	int failed = 0;
};

// A path for a log line. path::string() can throw on Windows for a name the ANSI code page cannot
// hold, and a log line is never worth an exception.
std::string show(const fs::path &p)
{
	try { return p.string(); } catch (...) { return "<unprintable path>"; }
}

void fail(Tally &t, const char *what, const fs::path &p, const std::error_code &ec)
{
	++t.failed;
	fprintf(stderr, "[gxmigrate] could not %s %s: %s\n", what, show(p).c_str(), ec.message().c_str());
}

// What is at p. An error while looking is a FAILURE, never "absent" (review of b101653df, P3): an
// absence settles a file, or marks the whole migration done, for good.
fs::file_type kindOf(const fs::path &p, Tally &t)
{
	std::error_code ec;
	const fs::file_type type = fs::status(p, ec).type();
	if (type == fs::file_type::none || type == fs::file_type::unknown)
		fail(t, "look at", p, ec ? ec : std::make_error_code(std::errc::io_error));
	return type;
}

// MapCache.ini, any case. native() rather than string(): no conversion, so no throw.
template <class C>
bool isMapCache(const std::basic_string<C> &name)
{
	static const char want[] = "mapcache.ini";
	if (name.size() != sizeof(want) - 1)
		return false;
	for (size_t i = 0; i < name.size(); ++i)
	{
		C c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = (C)(c - 'A' + 'a');
		if (c != (C)want[i])
			return false;
	}
	return true;
}

// Skirmish*.ini, any case: Skirmish.ini, SkirmishStats.ini, SkirmishStats_InstanceNN.ini. Printable
// ASCII only - those are the engine's names, and path::string() cannot throw on one.
template <class C>
bool isSkirmishIni(const std::basic_string<C> &name)
{
	static const char head[] = "skirmish", tail[] = ".ini";
	const size_t h = sizeof(head) - 1, tl = sizeof(tail) - 1, n = name.size();
	if (n < h + tl)
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		C c = name[i];
		if (c < 0x20 || c > 0x7E)
			return false;
		if (c >= 'A' && c <= 'Z')
			c = (C)(c - 'A' + 'a');
		if ((i < h && c != (C)head[i]) || (i >= n - tl && c != (C)tail[i - (n - tl)]))
			return false;
	}
	return true;
}

// Copy src to dst only if dst does not exist. The copy lands in <dst>.gxpart and is renamed into
// place, so an interrupted copy never leaves a truncated dst that every later pass would then skip
// as already there.
// Trade-off: one fixed .gxpart name, here and in mergeByKey and writeMarker. Two instances in the same
// first start (-multiinstance only; the single-instance mutex covers a normal start) share it, and
// nothing here stops one renaming the other's half-written file in and settling it. Per-process
// part names if -multiinstance first starts ever matter.
void copyIfAbsent(const fs::path &src, const fs::path &dst, Tally &t)
{
	std::error_code ec;
	if (fs::exists(dst, ec))
	{
		++t.present;
		return;
	}
	if (ec)
		return fail(t, "check", dst, ec);

	fs::path part = dst;
	part += ".gxpart";
	fs::copy_file(src, part, fs::copy_options::overwrite_existing, ec);
	if (!ec)
		fs::rename(part, dst, ec);
	if (ec)
	{
		std::error_code ignored;
		fs::remove(part, ignored);
		return fail(t, "copy", src, ec);
	}
	++t.copied;
}

bool readAll(const fs::path &p, std::string &out);

// *.sav, any case, as GameState::iterateSaveFiles lists them.
template <class C>
bool isSav(const std::basic_string<C> &name)
{
	static const char tail[] = ".sav";
	const size_t n = name.size();
	if (n < 4)
		return false;
	for (size_t i = 0; i < 4; ++i)
	{
		C c = name[n - 4 + i];
		if (c >= 'A' && c <= 'Z')
			c = (C)(c - 'A' + 'a');
		if (c != (C)tail[i])
			return false;
	}
	return true;
}

// Save\ (review of ccea6fbd1, P2). A save's name is not the player's: GameState::findNextSaveFilename
// numbers them from 00000000.sav, so a player who saved once on a recent build already holds the old
// folder's first name. An old .sav whose name holds other bytes takes the lowest free number, as the
// engine names a new save; one met on the way with the same bytes is this save, across already.
// Neither file is overwritten or dropped. Anything else in Save\ is a scratch map the engine writes out
// of a save on load (GameStateMap::clearScratchPadMaps), so it keeps copyIfAbsent.
void copySave(const fs::path &src, const fs::path &dst, Tally &t)
{
	std::error_code ec;
	if (!fs::exists(dst, ec) || !isSav(dst.filename().native()))	// an error looking: copyIfAbsent reports it
		return copyIfAbsent(src, dst, t);
	std::string mine;
	if (!readAll(src, mine))
		return fail(t, "read", src, std::make_error_code(std::errc::io_error));
	fs::path at = dst;
	for (int n = 0; n <= 99999999; ++n)	// GameState.cpp's MAX_SAVE_FILE_NUMBER
	{
		const fs::file_type kind = kindOf(at, t);
		if (kind == fs::file_type::not_found)
			return copyIfAbsent(src, at, t);
		if (kind == fs::file_type::none || kind == fs::file_type::unknown)
			return;		// kindOf counted the failure
		if (kind == fs::file_type::regular)
		{
			const auto size = fs::file_size(at, ec);
			if (ec)
				return fail(t, "look at", at, ec);
			std::string there;
			if (size == mine.size() && !readAll(at, there))
				return fail(t, "read", at, std::make_error_code(std::errc::io_error));
			if (size == mine.size() && there == mine)
			{
				++t.present;
				return;
			}
		}
		char name[16];
		snprintf(name, sizeof(name), "%08d.sav", n);
		at = dst.parent_path() / name;
	}
	fail(t, "find a free save name for", src, std::make_error_code(std::errc::file_exists));
}

// One folder of Maps or Replays, file by file. Each folder is opened here, so one that cannot be is
// a failure, never "empty" (review of 6ee23a741, P2). A folder behind a link - a symlink or an NTFS
// junction - is opened like any other: recursive_directory_iterator yielded the link and never went
// in, and the pass went done without its files.
//
// `inside` is the chain of folders being walked. A link back to one of them would never end, and
// what it leads to is being copied already, so it is left out. Only a link is compared: a plain
// folder cannot start a loop, and equivalent() is not asked of a filesystem that never needed it.
//
// Nor is a link to a folder that holds `to`: a link to Documents, to GeneralsX\ or to the
// new Maps itself walks into the folder being written and copies it into itself, a level deeper each
// time round, until the path is too long - a runaway copy that can fill the disk. `to`'s parents are
// the output and every folder above it, and each exists: this call's caller created them.
void copyDir(const fs::path &from, const fs::path &to, std::vector<fs::path> &inside, Tally &t, bool saves)
{
	std::error_code ec;
	if (fs::symlink_status(from, ec).type() != fs::file_type::directory)
	{
		std::vector<fs::path> avoid(inside);
		for (fs::path p = to.parent_path(); !p.empty(); p = p.parent_path())
		{
			avoid.push_back(p);
			if (!p.has_relative_path())
				break;
		}
		for (const fs::path &a : avoid)
		{
			const bool same = fs::equivalent(from, a, ec);
			if (ec)
				return fail(t, "follow", from, ec);
			if (same)
			{
				fprintf(stderr, "[gxmigrate] not following %s: it leads back into %s\n", show(from).c_str(), show(a).c_str());
				return;
			}
		}
	}
	fs::create_directories(to, ec);
	if (ec)
		return fail(t, "create", to, ec);
	fs::directory_iterator it(from, ec);
	if (ec)
		return fail(t, "read", from, ec);

	inside.push_back(from);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec))
	{
		const fs::path &p = it->path();
		// kindOf, not is_directory/is_regular_file: those answer false on a status error, and the
		// entry was skipped as if it were neither.
		const fs::file_type type = kindOf(p, t);
		if (type == fs::file_type::directory)
			copyDir(p, to / p.filename(), inside, t, saves);
		else if (type == fs::file_type::regular && !isMapCache(p.filename().native()))
			(saves ? copySave : copyIfAbsent)(p, to / p.filename(), t);
	}
	inside.pop_back();
	if (ec)
		fail(t, "read", from, ec);
}

void copyTree(const fs::path &from, const fs::path &to, Tally &t)
{
	if (kindOf(from, t) != fs::file_type::directory)
		return;		// nothing to bring across is not a failure; an error looking is (kindOf)
	std::vector<fs::path> inside;
	copyDir(from, to, inside, t, to.filename() == "Save");
}

// The whole file, or false (review of 6ee23a741, P2). A failed open sets only failbit, never badbit,
// and a read error inside the filebuf looks like the end of the file, so the bytes are also counted
// against the size. A file that changes while it is read fails too; the next start retries.
bool readAll(const fs::path &p, std::string &out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f.is_open())
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	std::error_code ec;
	const auto size = fs::file_size(p, ec);
	return !ec && !f.bad() && out.size() == size;
}

// GeneralsX: A UTF-8 BOM at the start of a file (a hand edit in an older Notepad) is not part of its
// first key - the rule the game (UserPreferences::load) and the launcher (gxio strip_bom) read Options.ini by. mergeByKey read
// through it: a BOM-first key in the new file counted as absent, so the old value was appended after it and the game (the last
// line wins) ran the old one while the launcher showed the new; and a BOM-first old line carried its BOM into the middle of the
// new file. The keys are compared, and the old lines copied, past it; the new file keeps its own.
std::string withoutLeadingBom(const std::string &s)
{
	if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		return s.substr(3);
	return s;
}

// The key of an ini line: the text before '=', trimmed, lowercased. "" for a line with no key.
std::string keyOf(std::string line)
{
	const size_t eq = line.find('=');
	if (eq == std::string::npos)
		return std::string();
	line.erase(eq);
	const size_t b = line.find_first_not_of(" \t");
	const size_t e = line.find_last_not_of(" \t\r");
	if (b == std::string::npos)
		return std::string();
	line = line.substr(b, e - b + 1);
	for (char &c : line)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return line;
}

// A SagePatch.ini line is a candidate only inside a GameData block, the way INI::load reads it: a
// line with '=' is a field, End closes the block, any other line opens one.
struct GameDataScan
{
	bool in = false;
	bool field(std::string line)
	{
		const size_t b = line.find_first_not_of(" \t");
		if (b == std::string::npos || line[b] == ';' || line[b] == '\r')
			return false;
		line.erase(0, b);
		for (char &c : line)
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
		const auto word = [&](const char *w, size_t n)
			{ return line.compare(0, n, w) == 0 && (line.size() == n || strchr(" \t\r;", line[n]) != nullptr); };
		if (line.find('=') != std::string::npos)
			return in;
		in = !word("end", 3) && word("gamedata", 8);
		return false;
	}
};

// The SagePatch.ini files that hold nobody's choice: what a launcher or the engine writes, untouched.
// Either side of the migration being one of them is treated as absent:
//   * NEW folder (review of 889749a00, P2): each has all six fields the engine reads, so a merge
//     into one could never bring an old camera value across. The old file replaces it whole.
//   * OLD folder (review of 2acfb7031, P2): there is nothing to bring across, and the new folder
//     keeps what it has. Without this an untouched pre-22/09 launcher file brought back its 210
//     minimum, and a pre-25/09 engine's first write its 350 maximum, over the current defaults.
// The first two are today's writers: both launchers at their startup (GameSagePatch.WriteCamera(100,
// 100) == gxio sagepatch::render) and the engine's first write (GameEngine.cpp). The rest are older
// writers' texts, still on disk in the old folder: APPEND-ONLY, never edit one. gxio's sagepatch.rs
// holds the first two equal to their writers and pins the rest.
const char *const kUntouchedSagePatch[] = {
R"GXDEFAULT(; GeneralsX camera / client tuning, written by the launcher. Safe to delete.
; The engine restricts this file to six client-side fields, so nothing here
; can affect the shared simulation or desync a match.
GameData
  MaxCameraHeight = 310.0
  MinCameraHeight = 100.0
  EnforceMaxCameraHeight = No
  KeyboardScrollSpeedFactor = 1.0
  TerrainDrawDistanceScale = 1.05
  ShellMapSoundVolumeScale = 50%
End
)GXDEFAULT",
R"GXDEFAULT(; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Vanilla ceiling (310), the same maximum both launchers write.
  MaxCameraHeight = 310.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
  ; Shell map (menu background) world SFX at half volume; music and UI untouched.
  ShellMapSoundVolumeScale = 50%
End
)GXDEFAULT",
// The engine's first write before 0af21a6f9 (gxio ENGINE_AUTOWRITE_BLOCK_V1).
R"GXDEFAULT(; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Slightly higher than vanilla (310); further out without seeing past the map border.
  MaxCameraHeight = 350.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
End
)GXDEFAULT",
// The engine's first write from 0af21a6f9 until 25/09/2026, its 350 maximum (gxio
// ENGINE_AUTOWRITE_BLOCK_V2).
R"GXDEFAULT(; -----------------------------------------------------------------------------
; SagePatch - Casual QoL overrides for GeneralsX
;
; Loaded by the engine after the BIG-archived Data/INI/GameData.ini, so values
; here override (not append to) the originals.
; -----------------------------------------------------------------------------

GameData
  ; Slightly higher than vanilla (310); further out without seeing past the map border.
  MaxCameraHeight = 350.0
  ; Slightly lower than vanilla (120) so casual zoom-in feels useful.
  MinCameraHeight = 100.0
  ; Still soft-disabled so the user can push past max without a hard clamp.
  EnforceMaxCameraHeight = No
  ; Keyboard scroll - vanilla 0.5 is sluggish, double it.
  KeyboardScrollSpeedFactor = 1.0
  ; ~5% more terrain drawn at max zoom to fix terrain pop-in.
  TerrainDrawDistanceScale = 1.05
  ; Shell map (menu background) world SFX at half volume; music and UI untouched.
  ShellMapSoundVolumeScale = 50%
End
)GXDEFAULT",
// Both launchers, 0af21a6f9 (10/09) to 70bc80d86 (22/09): the 210 minimum.
R"GXDEFAULT(; GeneralsX camera / client tuning, written by the launcher. Safe to delete.
; The engine restricts this file to six client-side fields, so nothing here
; can affect the shared simulation or desync a match.
GameData
  MaxCameraHeight = 310.0
  MinCameraHeight = 210.0
  EnforceMaxCameraHeight = No
  KeyboardScrollSpeedFactor = 1.0
  TerrainDrawDistanceScale = 1.05
  ShellMapSoundVolumeScale = 50%
End
)GXDEFAULT",
// Both launchers, 9128f4a96 (10/08) to 0af21a6f9: five fields, the 210 minimum.
R"GXDEFAULT(; GeneralsX camera / client tuning, written by the launcher. Safe to delete.
; The engine restricts this file to five client-side fields, so nothing here
; can affect the shared simulation or desync a match.
GameData
  MaxCameraHeight = 310.0
  MinCameraHeight = 210.0
  EnforceMaxCameraHeight = No
  KeyboardScrollSpeedFactor = 1.0
  TerrainDrawDistanceScale = 1.05
End
)GXDEFAULT",
// The C# launcher's first camera screen, 5e863860b (10/08), before 9128f4a96 the same night.
R"GXDEFAULT(; GeneralsX camera / client tuning, written by the launcher. Safe to delete.
; The engine restricts this file to five client-side fields, so nothing here
; can affect the shared simulation or desync a match.
GameData
  MaxCameraHeight = 350.0
  MinCameraHeight = 100.0
  EnforceMaxCameraHeight = No
  KeyboardScrollSpeedFactor = 1.0
  TerrainDrawDistanceScale = 1.05
End
)GXDEFAULT",
};

// Since 0af21a6f9 every engine start splices this into a SagePatch.ini that lacks the key, just
// before its GameData block's End (GameEngine.cpp, sagePatchMigrations). So each text above without
// the key is also on disk WITH it. gxio's sagepatch.rs holds this equal to the engine's own row.
const char *const kShellMapSplice = R"GXSPLICE(  ; Shell map (menu background) world SFX at half volume; music and UI untouched.
  ShellMapSoundVolumeScale = 50%
)GXSPLICE";

// CRLF read as LF: C# and the engine's text-mode fprintf write CRLF on Windows, gxlauncher LF, and
// this source may be checked out either way.
std::string lf(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] != '\r' || i + 1 == s.size() || s[i + 1] != '\n')
			out += s[i];
	return out;
}

bool isUntouchedSagePatch(const std::string &text)
{
	const std::string t = lf(text);
	for (const char *d : kUntouchedSagePatch)
	{
		std::string u = lf(d);
		if (t == u)
			return true;
		if (u.find("ShellMapSoundVolumeScale") == std::string::npos
			&& t == u.insert(u.size() - strlen("End\n"), lf(kShellMapSplice)))
			return true;
	}
	return false;
}

// Copied if the new folder has none. Otherwise the old keys the new file lacks are appended, and
// every line already in the new file stays first, unchanged. See kPlainFiles for which files and why.
//
// gameData (SagePatch.ini): only keys from the old file's GameData block, appended as a GameData
// block of their own. A key left outside a block is an unknown top-level token, and the engine's
// INI loader throws on it - the game does not start (GameEngine.cpp, the sagePatch helpers).
void mergeByKey(const fs::path &oldIni, const fs::path &newIni, Tally &t, bool gameData)
{
	if (kindOf(oldIni, t) != fs::file_type::regular)
		return;
	std::string old;
	if (!readAll(oldIni, old))
		return fail(t, "read", oldIni, std::make_error_code(std::errc::io_error));
	if (gameData && isUntouchedSagePatch(old))
		return;		// nobody's choice in it: as if there were no old file (kUntouchedSagePatch)
	std::error_code ec;
	if (!fs::exists(newIni, ec))
		return copyIfAbsent(oldIni, newIni, t);

	std::string merged;
	if (!readAll(newIni, merged))
		return fail(t, "read", newIni, std::make_error_code(std::errc::io_error));

	if (gameData && isUntouchedSagePatch(merged))
		merged = old;	// as if absent: the old file comes across whole, as copyIfAbsent brings it
	else
	{
		// gameData: the new file's keys count only inside its GameData blocks too. A same-named key in
		// another block is not the old value's key.
		std::set<std::string> have;
		{
			std::istringstream in(withoutLeadingBom(merged));
			std::string line;
			GameDataScan scan;
			while (std::getline(in, line))
				if (!gameData || scan.field(line))
					have.insert(keyOf(line));
		}

		std::string add;
		std::istringstream in(withoutLeadingBom(old));
		std::string line;
		GameDataScan scan;
		while (std::getline(in, line))
		{
			const std::string k = keyOf(line);
			if ((gameData && !scan.field(line)) || k.empty() || !have.insert(k).second)
				continue;
			add += line;
			if (line.empty() || line.back() != '\r')
				add += '\r';
			add += '\n';
		}
		if (add.empty())
		{
			++t.present;
			return;
		}
		// A final line with no newline would otherwise be fused with the first appended key.
		if (!merged.empty() && merged.back() != '\n')
			merged += "\r\n";
		// Appended to an EMPTY file, the old lines become its start: the old file's own leading BOM goes first, so a second
		// BOM in it stays data rather than becoming the file's. Not SagePatch.ini (gameData): the
		// engine's INI::load does not skip a BOM, so "<BOM>GameData" would be an unknown block that stops the game.
		if (!gameData && merged.empty() && withoutLeadingBom(old).size() != old.size())
			merged = old.substr(0, 3);
		// Nor may a BOM that was DATA in the old file (not its first bytes - after a blank first line the loop skips) become
		// the new file's first bytes: a line break goes first (asm0967).
		if (!gameData && merged.empty() && withoutLeadingBom(add).size() != add.size())
			merged = "\r\n";
		merged += gameData ? "GameData\r\n" + add + "End\r\n" : add;
	}

	fs::path part = newIni;
	part += ".gxpart";
	FILE *fp = fopen(show(part).c_str(), "wb");
	if (fp != nullptr)
		fwrite(merged.data(), 1, merged.size(), fp);
	if (!gxCommitAndReplace(fp, show(part).c_str(), show(newIni).c_str()))
		return fail(t, "merge into", newIni, std::make_error_code(std::errc::io_error));
	++t.copied;
}

// The marker: `settled: <file>` per plain file, and a `result:` line once nothing is left to do.
struct Marker
{
	bool finished = false;
	std::set<std::string> settled;
};

// False when the marker is there but cannot be read (review of 6ee23a741, P2): its settled lines are
// then unknown, and a pass without them would bring back a file the player removed.
bool readMarker(const fs::path &p, Marker &m)
{
	Tally t;
	const fs::file_type type = kindOf(p, t);
	if (type == fs::file_type::not_found)
		return true;
	std::string text;
	if (type != fs::file_type::regular || !readAll(p, text))
		return false;
	std::istringstream f(text);
	std::string line;
	while (std::getline(f, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.rfind("result: ", 0) == 0)
			m.finished = true;
		else if (line.rfind("settled: ", 0) == 0)
			m.settled.insert(line.substr(9));
	}
	return true;
}

// Written aside and swapped in: a torn marker would drop its settled lines, and the next pass would
// then bring back a file the player removed.
bool writeMarker(const fs::path &newDir, const fs::path &oldDir, const Marker &m, const char *result)
{
	const fs::path marker = newDir / GX_MIGRATION_MARKER;
	fs::path part = marker;
	part += ".gxpart";
	FILE *fp = fopen(show(part).c_str(), "w");
	if (fp != nullptr)
	{
		fprintf(fp, "GeneralsX user data migration. Each file below came across once and is not looked at again.\n");
		fprintf(fp, "from: %s\n", show(oldDir).c_str());
		for (const std::string &name : m.settled)
			fprintf(fp, "settled: %s\n", name.c_str());
		if (result != nullptr)
			fprintf(fp, "result: %s\n", result);
		// Never "delete this file": until it is here, both launchers keep their settings in
		// the old folder (release.rs windows_settings_dir, OwnedPaths.UserDataDir), and whatever they
		// saved there meanwhile lost to this folder's keys at the re-run. A deletion also forgets every
		// settled line above. Without the result line, the next start runs Maps, Replays and Save again.
		fprintf(fp, "Keep this file: without it the launchers go back to the old folder's settings.\n");
		fprintf(fp, "To bring across maps, replays and saves again, delete only the result line. Nothing already here is overwritten.\n");
	}
	return gxCommitAndReplace(fp, show(part).c_str(), show(marker).c_str());
}

GXMigrateResult migrate(const fs::path &oldDir, const fs::path &newDir)
{
	Marker m;
	if (!readMarker(newDir / GX_MIGRATION_MARKER, m))
	{
		// Nothing is touched. Whether the identity is across is unknown, so none may be minted.
		fprintf(stderr, "[gxmigrate] could not read %s - nothing is done, and it runs again on the next start\n",
			show(newDir / GX_MIGRATION_MARKER).c_str());
		return GX_MIGRATE_IDENTITY_PENDING;
	}
	if (m.finished && m.settled.count(kSaveSettled) == 0)
	{
		Tally t;
		copyTree(oldDir / "Save", newDir / "Save", t);
		if (t.failed == 0)
			m.settled.insert(kSaveSettled);
		if (t.failed != 0 || !writeMarker(newDir, oldDir, m, "done"))
		{
			fprintf(stderr, "[gxmigrate] Save incomplete - it runs again on the next start\n");
			return GX_MIGRATE_PARTIAL;
		}
	}
	if (m.finished)
		return GX_MIGRATE_ALREADY;

	std::error_code ec;
	fs::create_directories(newDir, ec);

	Tally t;
	const fs::file_type oldKind = kindOf(oldDir, t);
	if (t.failed != 0)
	{
		// Could not even look. The identity may be in there, so nothing may be minted over it.
		fprintf(stderr, "[gxmigrate] incomplete - it runs again on the next start\n");
		return GX_MIGRATE_IDENTITY_PENDING;
	}
	if (oldKind != fs::file_type::directory)
	{
		m.settled.insert(kSaveSettled);
		if (!writeMarker(newDir, oldDir, m, "no old folder"))
		{
			fprintf(stderr, "[gxmigrate] could not write the marker in %s\n", show(newDir).c_str());
			return GX_MIGRATE_PARTIAL;
		}
		return GX_MIGRATE_NO_SOURCE;
	}

	std::vector<Plain> files(std::begin(kPlainFiles), std::end(kPlainFiles));
	std::vector<std::string> skirmish;	// owns the names files points at
	{
		fs::directory_iterator it(oldDir, ec);
		// The name first, so only a Skirmish*.ini whose type cannot be read is a failure.
		for (; !ec && it != fs::directory_iterator(); it.increment(ec))
			if (isSkirmishIni(it->path().filename().native()) && kindOf(it->path(), t) == fs::file_type::regular)
				skirmish.push_back(it->path().filename().string());
		if (ec)
			fail(t, "read", oldDir, ec);
	}
	for (const std::string &f : skirmish)
		files.push_back({ f.c_str(), COPY });

	std::set<std::string> copiedNow;	// the plain files this pass put into the new folder
	for (const Plain &p : files)
	{
		const char *f = p.name;
		if (m.settled.count(f) != 0)
			continue;
		const int failedBefore = t.failed, copiedBefore = t.copied;
		if (p.how != COPY)
			mergeByKey(oldDir / f, newDir / f, t, p.how == MERGE_GAMEDATA);
		else if (kindOf(oldDir / f, t) == fs::file_type::regular)
			copyIfAbsent(oldDir / f, newDir / f, t);
		if (t.failed == failedBefore)
			m.settled.insert(f);
		if (t.copied != copiedBefore)
			copiedNow.insert(f);
	}
	for (const char *d : kTrees)
		copyTree(oldDir / d, newDir / d, t);

	fprintf(stderr, "[gxmigrate] %s -> %s: %d copied, %d already there, %d failed\n",
		show(oldDir).c_str(), show(newDir).c_str(), t.copied, t.present, t.failed);
	// The settled files are recorded on a PARTIAL pass too; that record is what stops a retry
	// bringing them back.
	if (t.failed == 0)
		m.settled.insert(kSaveSettled);
	if (!writeMarker(newDir, oldDir, m, t.failed == 0 ? "done" : nullptr))
	{
		fprintf(stderr, "[gxmigrate] could not write the marker in %s\n", show(newDir).c_str());
		++t.failed;
		// Nothing this pass settled is on record, so the next start would copy again whatever this
		// session deletes, undoing a sign-out or an identity reset. The
		// sign-in and identity this pass copied are taken back - an identity taken back returns
		// IDENTITY_PENDING below, so none is minted - and the next start that can write the marker
		// brings both. Other files stay: taking them back would let this session's defaults win.
		// Trade-off: one settled as ALREADY THERE this pass is not ours to take back, so a sign-out of
		// it this session is still undone. A second place to record the marker would close that.
		for (const char *f : { kIdentity, kLogin })
			if (copiedNow.count(f) != 0)
			{
				m.settled.erase(f);
				fs::remove(newDir / f, ec);
				if (ec)
					fail(t, "take back", newDir / f, ec);
			}
	}
	if (m.settled.count(kIdentity) == 0)
	{
		fprintf(stderr, "[gxmigrate] %s has not come across yet - no new identity is made until it has\n", kIdentity);
		return GX_MIGRATE_IDENTITY_PENDING;
	}
	if (t.failed != 0)
	{
		fprintf(stderr, "[gxmigrate] incomplete - it runs again on the next start\n");
		return GX_MIGRATE_PARTIAL;
	}
	return GX_MIGRATE_DONE;
}

} // namespace

std::string gxLegacyUserDataDir(const char *newDir, const char *leaf)
{
	std::string p(newDir != nullptr ? newDir : "");
	while (!p.empty() && (p.back() == '/' || p.back() == '\\'))
		p.pop_back();
	char sep = '\\';
	for (int up = 0; up < 2; ++up)
	{
		const size_t cut = p.find_last_of("/\\");
		if (cut == std::string::npos || cut == 0)
			return std::string();
		sep = p[cut];
		p.erase(cut);
	}
	if (leaf != nullptr && leaf[0] != '\0')
	{
		p += sep;
		p += leaf;
	}
	return p;
}

GXMigrateResult gxMigrateUserData(const char *oldDir, const char *newDir)
{
	if (oldDir == nullptr || newDir == nullptr || oldDir[0] == '\0' || newDir[0] == '\0')
		return GX_MIGRATE_PARTIAL;
	try
	{
		return migrate(fs::path(oldDir), fs::path(newDir));
	}
	catch (...)
	{
		// Whether GXPlayer.ini is across is unknown here, so no identity may be minted over it: PARTIAL
		// let WinMain mint one, and every retry then found it already there.
		fprintf(stderr, "[gxmigrate] stopped by an unexpected error - it runs again on the next start\n");
		return GX_MIGRATE_IDENTITY_PENDING;
	}
}
