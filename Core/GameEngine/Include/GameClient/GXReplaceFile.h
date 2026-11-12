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

// Finish a write-to-tmp and put it in place, in one call: the writer opens "<file>.tmp" and hands the stream here.
// fopen(path, "w") truncates the live file first, so a crash or sync client mid-write sees a short or empty file.
// Flush, then replace: rename() does not order data against the rename. On ANY failure the .tmp is removed.
// POSIX rename() is atomic and inline; MSVCRT's refuses an existing file, so Windows uses gxPlatformReplaceFile.
#pragma once

#include <cerrno>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <process.h>
// Win32Device/Common/Win32ReplaceFile.cpp: MoveFileExA with MOVEFILE_REPLACE_EXISTING, on the
// same narrow path the caller opened the .tmp with through fopen(). Not compiled by MSVC on this
// branch yet; every executable that calls gxCommitAndReplace links the game's device library.
bool gxPlatformReplaceFile(const char *tmpPath, const char *finalPath);
#else
#include <sys/stat.h>
#include <unistd.h>
inline bool gxPlatformReplaceFile(const char *tmpPath, const char *finalPath)
{
	return rename(tmpPath, finalPath) == 0;
}
#endif

// A read that fails with ENOENT is a missing file
// ONLY when no name is there at all. A dangling symlink - a GXPlayer.ini linked onto a volume that is not mounted -
// also opens as ENOENT, and writing a new file over the NAME replaces the link: when the volume comes back, the
// player's file is no longer reached. Call this after an ENOENT (it may change errno). On Windows the directory entry
// is looked up with _findfirst, which does not follow a link.
inline bool gxNameExists(const char *path)
{
#ifdef _WIN32
	struct _finddata_t fd;
	const intptr_t h = _findfirst(path, &fd);
	if (h == -1)
		return false;
	_findclose(h);
	return true;
#else
	struct stat st;
	return lstat(path, &st) == 0;
#endif
}

// A NEW temp file beside finalPath, "<final>.<pid>.<n>.tmp", made
// exclusively (mode "x": refused if the name exists, so a link planted at that name is never followed and no other file
// is truncated through it, and two writers never share one). tmpOut gets the name; nullptr when none could be made.
// For the writers added with it; the older ones keep "<final>.tmp", which release.rs's writers contract pins.
inline FILE *gxCreateTempFor(const char *finalPath, std::string &tmpOut, const char *mode = "wbx")
{
#ifdef _WIN32
	const int pid = _getpid();
#else
	const int pid = (int)getpid();
#endif
	for (int n = 0; n < 16; ++n)
	{
		tmpOut = std::string(finalPath) + "." + std::to_string(pid) + "." + std::to_string(n) + ".tmp";
		errno = 0;
		FILE *f = fopen(tmpOut.c_str(), mode);
		if (f != nullptr)
			return f;
		if (errno != EEXIST)
			return nullptr;
	}
	return nullptr;
}

inline bool gxCommitAndReplace(FILE *fp, const char *tmpPath, const char *finalPath)
{
	if (fp == nullptr)
		return false;

	bool ok = (fflush(fp) == 0) && !ferror(fp);
#ifdef _WIN32
	ok = ok && (_commit(_fileno(fp)) == 0);
#else
	ok = ok && (fsync(fileno(fp)) == 0);
#endif
	ok = (fclose(fp) == 0) && ok;

	if (ok)
		ok = gxPlatformReplaceFile(tmpPath, finalPath);
	if (!ok)
		remove(tmpPath);
	return ok;
}

// Copy srcPath over finalPath without ever
// destroying finalPath first. The replay "save as" used to DELETE the saved replay it was replacing and then copy
// LastReplay.rep over it, so a copy that failed - the source unreadable or gone, a full disk - had already lost the
// player's replay. The source is opened before anything else, the copy goes to "<final>.tmp", and only a complete,
// flushed copy is swapped in. False, with finalPath exactly as it was, on any failure.
// (gxCreateTempFor is defined above gxCommitAndReplace.)
inline bool gxCopyAndReplace(const char *srcPath, const char *finalPath)
{
	FILE *in = fopen(srcPath, "rb");
	if (in == nullptr)
		return false;
	// A fresh exclusive temp, never "<final>.tmp": a link left at that name would truncate whatever it points at
	std::string tmp;
	FILE *out = gxCreateTempFor(finalPath, tmp);
	if (out == nullptr)
	{
		fclose(in);
		return false;
	}
	char buf[16384];
	size_t n;
	bool ok = true;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	ok = ok && !ferror(in);
	fclose(in);
	if (!ok)
	{
		fclose(out);
		remove(tmp.c_str());
		return false;
	}
	return gxCommitAndReplace(out, tmp.c_str(), finalPath);
}
