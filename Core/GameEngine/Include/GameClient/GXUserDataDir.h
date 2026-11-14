/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GX_USERDATA_DIR moves the whole user-data folder so A/B arms and scripted peers never touch a real profile.
// Absolute only (scripts/test/userdata-dir-test.cpp); SET BUT UNUSABLE (empty, relative) STOPS THE PROCESS (exit 2).
// Never name the real profile folder with it: the override skips the one-time Windows migration on that folder.
// gxlaunch.exe and the C# loader ignore it. Ask via gxUserDataDirOverride(), never getenv().
// Not in the simulation digest, but a stale MapCache.ini (user maps' start waypoints) can desync a fresh-profile arm.
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
		// \\server\share, never \\server alone: a UNC
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
		MessageBoxA(NULL, line.c_str(), "Generals Vanguard", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
	std::_Exit(2);
}

/// iOS's stderr mirror folder. SDL3Main installs that mirror long before GlobalData exists, in
/// $HOME/Documents/ where the Files app reaches it, and it rotates generals-stderr.log on every
/// launch. Under an override that APPLIES it goes in the override's folder instead, or two test
/// launches delete the real profile's last log, the desync evidence. Prints
/// nothing: SDL3Main's later gxUserDataDirOverride() call says the verdict, into the mirror. A refused
/// value stops the process here, before the real log is rotated.
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

/// The previous Windows user-data folder, Documents\<UserDataLeafName>, no trailing
/// separator. WinMain sets it when it runs the one-time migration (GXUserDataMigrate.h), so never
/// under GX_USERDATA_DIR; it is empty on every other platform and in the tools.
///
/// Only parseMod (CommandLine.cpp) reads it. A relative -mod name resolves in the user-data folder,
/// and mods are not migrated - they can be any file or folder name - so a mod kept in the old folder
/// would not be found and the game would start without it, silently. parseMod looks here when the
/// new folder lacks the name. Read only: the engine never writes into a mod.
inline std::string &gxLegacyUserDataDirForMods()
{
	static std::string dir;
	return dir;
}
