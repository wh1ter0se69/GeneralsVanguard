// GeneralsX: GX_USERDATA_DIR: the whole user-data folder somewhere else.
//
// Owner decision B, 23/09/2026. A/B arms and scripted peers must never read or write the owner's
// profile. Options.ini, Network.ini, Replays, Maps, MapCache.ini, GXLogin.txt (his sign-in),
// GXPlayer.ini (his identity and friends) and the stderr mirror all live in the user-data folder,
// and all of them are reached through GlobalData::getPath_UserData() - except iOS's stderr mirror,
// installed before GlobalData exists, which asks gxIosStderrLogDir() below. So
//
//   GX_USERDATA_DIR=C:\dev\GX-AB\<arm>\userdata\      Windows
//   GX_USERDATA_DIR=$HOME/gx-ab/<arm>/userdata        macOS, Linux; iOS through devicectl -e
//
// moves every one of them. Both games' GlobalData.cpp ask gxUserDataDirOverride() FIRST, before the
// known-folder, HOME or XDG lookup, so nothing can win over it.
//
// The rules, each held by scripts/test/userdata-dir-test.cpp:
//   * Absolute only. Windows: X:\ or X:/, or \\server\share. POSIX: a leading /. No ~ expansion; that is
//     the shell's job.
//   * SET BUT UNUSABLE STOPS THE PROCESS (wh1ter0se, 28/09/2026). An empty or relative value is REFUSED with a
//     stderr line and the process exits 2 before anything reads or writes a profile
//     (gxUserDataDirRefuse). It used to fall back to the normal folder - the owner's, the one outcome a
//     test profile exists to prevent. Unset is still the normal player path.
//   * Exactly one trailing separator, the platform's own. The engine concatenates file names on.
//   * Every missing parent is created. Win32 CreateDirectory makes one level.
//   * A folder that cannot be created is STILL USED, and said so. Once an absolute folder has been
//     named, falling back to the owner's is the one outcome a test profile exists to prevent.
//   * WinMain skips the one-time Windows migration when the override APPLIES (GXUserDataMigrate.h):
//     a test run never copies the owner's files. A refused value never reaches it: GlobalData's call
//     stopped the process first.
//   * NEVER name the owner's own folder (Documents\GeneralsX\GeneralsZH\) with it. The override
//     applies, so the migration is skipped on the very folder it would fill, and a GXPlayer.ini
//     minted there unheld stops the old identity ever coming across. Nothing detects it
//     (review of 2acfb7031, P3).
//
// THE ENGINE AND gxlauncher FOLLOW IT (gxlauncher since 28/09/2026: its settings, its own state and
// WebView2's data, on this file's rules - scripts/launcher-rs gxio release.rs user_data_dir_override,
// held to scripts/test/userdata-dir-test.cpp's table). Both STOP on a set but unusable value (gxlauncher:
// user_data_dir_refusal, first thing in main()). gxlaunch.exe and the C# loader do NOT: they
// resolve the owner's folder on their own - gxlaunch.c reads and writes RelayAddress in the owner's
// Options.ini and reads the owner's GXLogin.txt. An arm or peer must start the game executable or
// gxlauncher, never those two, for "never the owner's profile" to hold (review of 2acfb7031, P3).
//
// To ask "am I on a test profile", call gxUserDataDirOverride(), never getenv(): it is also the call that
// stops a process whose value is refused.
//
// NOT IN THE SIMULATION DIGEST: Include/GameClient is not a digested root, though the digested
// GlobalData.cpp includes it (GXReplaceFile.h is the precedent). It picks a folder and adds no
// simulation code. The folder's CONTENTS are not all inert: MapCache.ini supplies user maps' start
// waypoints (Core MapUtil.cpp, GameLogic.cpp's start pick) and no identity field covers it, so a
// fresh-profile arm against a peer with a stale MapCache.ini can desync instead of being refused.
// That predates this header. Pure C++ and std::filesystem, no engine headers, tested on any host.
#pragma once

#ifdef _WIN32
#include <windows.h>	// MessageBoxA, GetStdHandle: already included by every Windows file that includes this
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#define GX_USERDATA_DIR_ENV "GX_USERDATA_DIR"

enum GXUserDataDirStatus
{
	GX_USERDATA_DIR_UNSET = 0,	///< not in the environment: the normal folder, nothing said
	GX_USERDATA_DIR_OK,			///< an absolute folder: use it
	GX_USERDATA_DIR_EMPTY,		///< set, but to nothing: refused
	GX_USERDATA_DIR_RELATIVE	///< not absolute on these rules: refused
};

/// Pure: no environment and no disk. `windows` picks the rules, so both are tested on any host.
/// On OK `dir` is the folder with one trailing separator; otherwise it is empty.
inline GXUserDataDirStatus gxResolveUserDataDir(const char *value, bool windows, std::string &dir)
{
	dir.clear();
	if (value == nullptr)
		return GX_USERDATA_DIR_UNSET;
	if (value[0] == '\0')
		return GX_USERDATA_DIR_EMPTY;

	const std::string v = value;
	auto isSep = [windows](char c) { return c == '/' || (windows && c == '\\'); };
	bool absolute;
	if (windows)
	{
		// GeneralsX: \\server\share, never \\server alone: a UNC
		// path names no folder without its share, so creating it fails and the engine would run on a profile
		// that cannot exist while WinMain skips the normal migration.
		size_t server = 2;
		while (server < v.size() && !isSep(v[server]))
			++server;
		absolute = (v.size() >= 3 && isalpha((unsigned char)v[0]) && v[1] == ':' && isSep(v[2]))	// X:\ or X:/
			|| (v.size() >= 3 && isSep(v[0]) && isSep(v[1]) && !isSep(v[2])					// \\server
				&& server + 1 < v.size() && !isSep(v[server + 1]));							// \share
	}
	else
		absolute = v[0] == '/';
	if (!absolute)
		return GX_USERDATA_DIR_RELATIVE;

	size_t end = v.size();
	while (end > 0 && isSep(v[end - 1]))
		--end;
	dir = v.substr(0, end);
	dir += windows ? '\\' : '/';
	return GX_USERDATA_DIR_OK;
}

/// GX_USERDATA_DIR from the environment, on this platform's rules, with the folder created.
/// Returns true with `dir` set when the override applies. `line` gets one stderr line, without the
/// newline, for every outcome except unset, which leaves it empty.
inline bool gxUserDataDirFromEnv(std::string &dir, std::string &line)
{
#ifdef _WIN32
	const bool windows = true;
#else
	const bool windows = false;
#endif
	const char *value = getenv(GX_USERDATA_DIR_ENV);
	line.clear();
	switch (gxResolveUserDataDir(value, windows, dir))
	{
	case GX_USERDATA_DIR_UNSET:
		return false;
	case GX_USERDATA_DIR_EMPTY:
		line = "[gx] GX_USERDATA_DIR is set but empty - REFUSED, not starting: unset it for the normal user-data folder";
		return false;
	case GX_USERDATA_DIR_RELATIVE:
		line = std::string("[gx] GX_USERDATA_DIR=") + value
			+ " is not an absolute path - REFUSED, not starting: name an absolute folder, or unset it";
		return false;
	case GX_USERDATA_DIR_OK:
		break;
	}

	std::error_code made, looked;
	std::filesystem::create_directories(std::filesystem::path(dir), made);
	if (std::filesystem::is_directory(std::filesystem::path(dir), looked))
		line = "[gx] GX_USERDATA_DIR: user data in " + dir;
	else
		line = "[gx] GX_USERDATA_DIR: " + dir + " could not be created (" + made.message()
			+ ") - used anyway, never the normal folder";
	return true;
}

/// A refused GX_USERDATA_DIR ends the process: `line` to stderr, then exit 2. A Win32 GUI process's stderr goes
/// nowhere unless the caller redirected it, so without one it is also a dialog - and only then: a modal box
/// under a harness that captures stderr is a hang nobody can click (gxlaunch.c's launch_game rule).
/// std::_Exit, not exit(): no static destructors, which crash this engine at exit (SDL3Main.cpp's _exit).
[[noreturn]] inline void gxUserDataDirRefuse(const std::string &line)
{
	fprintf(stderr, "%s\n", line.c_str());
	fflush(stderr);
#ifdef _WIN32
	const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err == NULL || err == INVALID_HANDLE_VALUE)
		MessageBoxA(NULL, line.c_str(), "GeneralsX", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
	std::_Exit(2);
}

/// iOS's stderr mirror folder. SDL3Main installs that mirror long before GlobalData exists, in
/// $HOME/Documents/ where the Files app reaches it, and it rotates generals-stderr.log on every
/// launch. Under an override that APPLIES it goes in the override's folder instead, or two test
/// launches delete the owner's last log, the desync evidence (review of 459e3ade2, P2). Prints
/// nothing: SDL3Main's later gxUserDataDirOverride() call says the verdict, into the mirror. A refused
/// value stops the process here, before the owner's log is rotated.
inline std::string gxIosStderrLogDir(const char *home)
{
	std::string dir, line;
	if (gxUserDataDirFromEnv(dir, line))
		return dir;
	if (!line.empty())
		gxUserDataDirRefuse(line);
	return std::string(home) + "/Documents/";
}

/// The engine's call: both games' GlobalData.cpp, and SDL3Main.cpp's DXVK folder and iOS Options.ini seed. As
/// gxUserDataDirFromEnv, and prints its line to stderr the first time there is one; a refused value
/// stops the process (gxUserDataDirRefuse). Every
/// GlobalData instance asks, overrides included, so it is said once per process, not per call.
/// On Windows that first print goes nowhere, because stderr is kept only once WinMain installs the
/// mirror, after GlobalData exists. WinMain prints the verdict again for that reason.
inline bool gxUserDataDirOverride(std::string &dir)
{
	static bool s_said = false;
	std::string line;
	const bool use = gxUserDataDirFromEnv(dir, line);
	if (!use && !line.empty())
		gxUserDataDirRefuse(line);
	if (!line.empty() && !s_said)
	{
		fprintf(stderr, "%s\n", line.c_str());
		s_said = true;
	}
	return use;
}

/// The Windows folder GeneralsX used before 23/09/2026, Documents\<UserDataLeafName>, no trailing
/// separator. WinMain sets it when it runs the one-time migration (GXUserDataMigrate.h), so never
/// under GX_USERDATA_DIR; it is empty on every other platform and in the tools.
///
/// Only parseMod (CommandLine.cpp) reads it. A relative -mod name resolves in the user-data folder,
/// and mods are not migrated - they can be any file or folder name - so a mod kept in the old folder
/// was no longer found and the game started without it, silently (review of the merged folder move,
/// finding 2). parseMod now looks here when the new folder lacks the name. Read only: the engine
/// never writes into a mod.
inline std::string &gxLegacyUserDataDirForMods()
{
	static std::string dir;
	return dir;
}
