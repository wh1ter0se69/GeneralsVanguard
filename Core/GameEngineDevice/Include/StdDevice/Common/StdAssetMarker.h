/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// GeneralsX @test Claude 09/09/2026 The asset-marker scan, extracted so it can be RUN.
//
// WHY THIS IS A HEADER. The refusal in StdBIGFileSystem.cpp decides whether the game starts at
// all, and it was written with no way to execute it: the logic was a file-static inside a
// translation unit that pulls in most of the engine, so proving it works meant building the whole
// thing. It was committed tagged EXPECTED for exactly that reason.
//
// Header-only and dependency-free, so scripts/test/asset-marker-test.cpp can drive it against real
// directories with a plain `c++ -std=c++17 -I Core/GameEngineDevice/Include` and no cmake, no
// engine and no network - the same shape as ChatRateLimit.h and its test.
//
// Core/GameEngineDevice/Include is in NO sim-digest path (checked: "GameEngineDevice" appears
// nowhere in resources/gitinfo/simsourcedigest_watcher.cmake), so moving this here cannot cost an
// atomic four-platform rebuild.

#pragma once

#ifdef _WIN32
#include "Utility/dirent_compat.h"
#else
#include <dirent.h>
#endif

namespace GXAssetMarker
{

/// Case-insensitive ASCII compare, deliberately NOT strcasecmp.
///
/// strcasecmp folds per LC_CTYPE. Under a Turkish locale 'I' folds to a dotless i and "INI.big"
/// stops matching itself - so a player in tr_TR would be told their game files are missing while
/// looking straight at them. Archive names are ASCII by construction, so a fixed A-Z fold is both
/// correct and immune to the locale. This mirrors asciiEqualNoCase in StdBIGFileSystem.cpp, which
/// made the same choice for the same reason.
inline bool asciiEqualNoCase(const char* a, const char* b)
{
	if (a == 0 || b == 0) {
		return false;
	}
	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;
		if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
		if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
		if (ca != cb) {
			return false;
		}
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

/// THREE outcomes, not two, and the third is the whole reason this is an enum.
///
/// "I looked and it is not there" and "I could not look" are different facts that deserve
/// different behaviour, and collapsing them is how a permissions problem gets reported to a player
/// as "your game files are missing" - sending them to hunt for data they already own. The caller
/// decides what to do about Unreadable; this function's job is only to be honest about which
/// happened.
enum Result
{
	NotFound = 0,
	Found = 1,
	Unreadable = 2
};

/// Does `dir` contain a file named `wanted`, in any capitalisation?
///
/// SCANS rather than probing spellings. The obvious implementation tests "INIZH.big" and
/// "inizh.big" and stops, which is enough to RECOGNISE a folder but not to REFUSE one: on a
/// case-sensitive filesystem a player holding "IniZH.big" would be refused over capitalisation.
/// A refusal is exactly where the more careful primitive belongs.
inline Result dirHasMarker(const char* dir, const char* wanted)
{
	if (dir == 0 || *dir == '\0' || wanted == 0) {
		return NotFound;
	}
	DIR* d = opendir(dir);
	if (d == 0) {
		return Unreadable;
	}
	Result r = NotFound;
	struct dirent* entry;
	while ((entry = readdir(d)) != 0) {
		if (asciiEqualNoCase(entry->d_name, wanted)) {
			r = Found;
			break;
		}
	}
	closedir(d);
	return r;
}

/// The marker each build requires: Zero Hour ships INIZH.big, base Generals ships INI.big.
///
/// A Zero Hour folder carries INI.big too, so the base build accepting it is correct rather than
/// sloppy - the base game genuinely runs from either, and refusing a superset would break installs
/// that work today.
inline const char* requiredMarkerFor(bool zeroHour)
{
	return zeroHour ? "inizh.big" : "ini.big";
}

} // namespace GXAssetMarker
