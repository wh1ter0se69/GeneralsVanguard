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

// GeneralsX: "Have I already mounted this archive?" - by FILE, not by
// the string somebody happened to spell it with.
//
// ArchiveFileSystem::m_archiveFileMap is keyed by the full path string of the archive
// (StdBIGFileSystem.cpp / Win32BIGFileSystem.cpp, the `m_archiveFileMap[(*it)] = archiveFile`
// line at the bottom of loadBigFilesFromDirectory). One archive reached through two spellings
// is therefore two entries: loadIntoDirectoryTree runs twice, the directory tree carries a
// duplicate location for every path in the archive, the first ArchiveFile is leaked because
// operator[] overwrites without deleting, and assetID - which hashes the map's basenames with
// no dedup (ArchiveFileSystem.cpp:251-308) - counts the archive twice.
//
// This has been fixed once before, narrowly. StdBIGFileSystem.cpp:805-828 (27/07/2026) strips
// trailing separators before joining "/../Generals", because "...Zero Hour/" + "/../Generals"
// spelled the same directory differently from the recursive scan and "the same 15 base-Generals
// archives got mounted TWICE under two spellings: 55 mounts for 40 files on disk". That fix
// covers exactly one join. Every other route to the same directory - the sibling scan, the
// Steam ZH_Generals subdirectory, a relative path from StdLocalFileSystem's recursive walk, a
// symlink - produces its own spelling and is not covered by it.
//
// So the test is moved off the string and onto the file. realpath()/_fullpath() collapse "..",
// ".", repeated separators and (on POSIX) symlinks, which is what makes two spellings of one
// archive compare equal.
//
// WHY A HEADER. Same reason as StdAssetMarker.h next door: the decision used to be a file-static
// inside a translation unit that pulls in most of the engine, so proving it meant building the
// whole thing. Header-only and dependency-free, so scripts/test/archive-mount-dedupe-test.cpp
// can drive it against real directories with a plain
// `c++ -std=c++17 -I Core/GameEngineDevice/Include` - no cmake, no engine, no network.
// It is also shared: StdBIGFileSystem and Win32BIGFileSystem are twins and both need it, so
// one copy is the point rather than a side effect.
//
// Core/GameEngineDevice/Include is in NO sim-digest path ("GameEngineDevice" appears nowhere in
// resources/gitinfo/simsourcedigest_watcher.cmake), so this cannot move sourceID.

#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#include <io.h>
#else
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace GXArchiveMount
{

/// The canonical absolute path of an EXISTING file or directory; empty when it does not exist
/// or cannot be resolved. Non-existence returning empty is deliberate - a path we cannot
/// resolve must never compare equal to another one we cannot resolve either.
inline std::string canonicalPath(const char* path)
{
	if (path == nullptr || path[0] == '\0') {
		return std::string();
	}

#ifdef _WIN32
	// Trade-off: _fullpath collapses "..", "." and separators but NOT junctions or symlinks -
	// Windows would need GetFinalPathNameByHandle for that. Every duplicate spelling this fixes
	// on Windows is a "..", separator or case difference, which _fullpath does collapse. Upgrade
	// to GetFinalPathNameByHandle if a junctioned install ever turns up.
	char buffer[4096] = { 0 };
	if (_fullpath(buffer, path, sizeof(buffer)) == nullptr) {
		return std::string();
	}
	if (_access(buffer, 0) != 0) {
		return std::string();
	}
	return std::string(buffer);
#else
	char* real = realpath(path, nullptr);
	if (real == nullptr) {
		return std::string();
	}
	std::string resolved(real);
	free(real);
	return resolved;
#endif
}

/// Do two CANONICAL paths name the same file?
///
/// POSIX compares exactly. realpath() already returns the on-disk case where the filesystem folds
/// it (measured on APFS: "rp/sub/file.BIG" resolves to ".../rp/Sub/File.big"), so one file never
/// canonicalises to two spellings there; and on a case-sensitive filesystem - Linux, iOS,
/// case-sensitive APFS - paths differing only by case ARE different files. Folding here could only
/// ever merge two real archives and drop one. FilenameList's case-insensitive set (FileSystem.h:66)
/// collapses "INI.big"/"ini.big" within ONE scan; this compares across scans, where it must not.
///
/// Windows folds: _fullpath does not canonicalise case, and NTFS is case-insensitive by default.
inline bool samePath(const std::string& lhs, const std::string& rhs)
{
	if (lhs.empty() || rhs.empty()) {
		return false;
	}
#ifdef _WIN32
	return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
#else
	return lhs == rhs;
#endif
}

/// Is the archive named `archiveName` one the -mod argument supplied? `modBIG` is the single-file
/// form, `modDir` the directory form (every *.big under it, recursively); either may be empty.
/// GeneralsX: Both sides go through
/// canonicalPath, so `-mod /game/./`, `/game/mod/../mod`, a trailing slash and a case spelling the
/// filesystem folds all name the same archives as the clean path. A raw string prefix test here let
/// an odd spelling defeat INI::loadFileDirectory's suppression of the patch's split command files.
/// A directory that resolves to the working directory (the game folder, `-mod .`) names no mod: it
/// would make every stock archive a mod archive. A path that does not resolve matches nothing.
inline bool isModArchive(const char* archiveName, const char* modBIG, const char* modDir)
{
	// The engine stores a -mod DIRECTORY with a trailing backslash ("/g/mod\\", CommandLine parseMod) and the
	// engine's paths mix '\\' and '/': off Windows a backslash is a separator here too, or realpath() cannot
	// resolve the argument and no archive ever matches (measured: every spelling without a trailing '/').
	auto slashes = [](const char* p) {
		std::string k(p != nullptr ? p : "");
#ifndef _WIN32
		for (char& ch : k) {
			if (ch == '\\') {
				ch = '/';
			}
		}
#endif
		return k;
	};
	const std::string archiveKey = slashes(archiveName), modBIGKey = slashes(modBIG), modDirKey = slashes(modDir);
	archiveName = archiveKey.c_str();
	modBIG = modBIGKey.c_str();
	modDir = modDirKey.c_str();

	const std::string archive = canonicalPath(archiveName);
	if (archive.empty()) {
		return false;
	}
	if (modBIG != nullptr && modBIG[0] != '\0' && samePath(archive, canonicalPath(modBIG))) {
		return true;
	}
	if (modDir != nullptr && modDir[0] != '\0') {
		std::string dir = canonicalPath(modDir);
		// The working directory is the game folder: a -mod directory that IS it, or that CONTAINS it (`-mod ..`),
		// would turn every stock and patch archive into a mod archive. Such an argument names no mod.
		const std::string cwd = canonicalPath(".");
		if (dir.empty() || samePath(dir, cwd)) {
			return false;
		}
#ifdef _WIN32
		const char sep = '\\';
#else
		const char sep = '/';
#endif
		if (dir.back() != sep) {
			dir += sep;
		}
		if (cwd.size() > dir.size()) {
#ifdef _WIN32
			if (_strnicmp(cwd.c_str(), dir.c_str(), dir.size()) == 0) {
				return false;
			}
#else
			if (cwd.compare(0, dir.size(), dir) == 0) {
				return false;
			}
#endif
		}
		if (archive.size() > dir.size()) {
#ifdef _WIN32
			return _strnicmp(archive.c_str(), dir.c_str(), dir.size()) == 0;
#else
			return archive.compare(0, dir.size(), dir) == 0;
#endif
		}
	}
	return false;
}

/// Index of the entry in `mountedKeys` naming the same file as `candidatePath`, or -1.
///
/// `mountedKeys` are the raw m_archiveFileMap keys, exactly as they were inserted - a mix of
/// absolute paths, paths carrying "..", and (from StdLocalFileSystem's recursive walk, which
/// passes the bare subdirectory name as currentDirectory with an empty originalDirectory,
/// StdLocalFileSystem.cpp:364-366) paths relative to the current working directory. All three
/// go through canonicalPath, so the comparison is between files rather than spellings.
///
/// A key that no longer resolves - deleted under us, or relative while the cwd has moved - is
/// skipped rather than treated as a match. Failing to dedupe is the old behaviour; wrongly
/// deduping would DROP an archive, which is strictly worse.
inline int findMounted(const std::vector<std::string>& mountedKeys, const char* candidatePath)
{
	const std::string candidate = canonicalPath(candidatePath);
	if (candidate.empty()) {
		return -1;
	}

	for (size_t i = 0; i < mountedKeys.size(); ++i) {
		if (samePath(canonicalPath(mountedKeys[i].c_str()), candidate)) {
			return (int)i;
		}
	}

	return -1;
}

/// loadBigFilesFromDirectory's handling of one scanned archive. FALSE when `candidatePath` is not
/// mounted yet - the caller opens and mounts it exactly as before.
///
/// TRUE when it already is, under any spelling. The archive is then NOT reopened, but everything
/// the reopen used to do FOR THE CALLER still happens, because callers depend on it:
///   * `loaded` is set. The directory did provide that archive, and loadBigFilesFromDirectory's
///     result is what tryLoadBigFiles reports. FALSE there sends loadBaseGeneralsAssetsForZH on to
///     lower-priority installs: with the Steam layout, whose ZH_Generals/ the primary recursive scan
///     already mounted, that means a SECOND base tree from a sibling folder, or a false "no base
///     Generals assets found" warning.
///   * with `overwrite` - ArchiveFileSystem::loadMods, a -mod directory - `promoteExisting(key)` is
///     called with the key the archive IS mounted under, so the caller re-inserts that archive at
///     the front and the mod still wins, exactly as the reopened copy used to. Without overwrite
///     nothing moves: a copy appended behind the one already mounted never became instance 0.
template <typename PromoteFn>
inline bool handleAlreadyMounted(const std::vector<std::string>& mountedKeys, const char* candidatePath,
                                 bool overwrite, bool& loaded, PromoteFn promoteExisting)
{
	const int existing = findMounted(mountedKeys, candidatePath);
	if (existing < 0) {
		return false;
	}
	if (overwrite) {
		promoteExisting(mountedKeys[existing]);
	}
	loaded = true;
	fprintf(stderr, "[ARCHIVES] already mounted as '%s', not reopening '%s'%s\n",
		mountedKeys[existing].c_str(), candidatePath, overwrite ? " (moved to the front for -mod)" : "");
	return true;
}

} // namespace GXArchiveMount
