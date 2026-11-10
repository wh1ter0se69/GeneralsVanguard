// GeneralsX: finish a write-to-tmp and put it in place, in one call.
//
// Options.ini, MapCache.ini and GXPlayer.ini used to be written with fopen(path, "w"). That
// truncates the live file before the first byte of the new one exists, so a crash, a full disk,
// or a sync client reading mid-write sees a short or empty file. On Windows the user-data folder
// is under Documents, which OneDrive Known Folder Move keeps under sync. GXPlayer.ini is the
// player's identity, so an empty one mints a new player.
//
// The writer opens "<file>.tmp" and hands the stream here. This flushes it to the disk, closes it,
// and replaces the real file. The flush-then-replace order is the point: MoveFileEx and rename()
// do not order the data against the rename, and without the flush a power cut can leave the new
// name holding nothing. On ANY failure the .tmp is removed and false is returned, and the real
// file is left exactly as it was.
//
// POSIX rename() replaces an existing file atomically, and it is ISO C, so that arm stays inline.
// MSVCRT's rename() REFUSES an existing destination (GXLoginFlow.cpp records what that cost), so
// Windows replaces through the Win32 API - and that arm lives in the platform layer,
// Core/GameEngineDevice/Source/Win32Device/Common/Win32ReplaceFile.cpp, declared here as
// gxPlatformReplaceFile (review of 5cdc742e1: platform code stays out of shared engine headers).
// The _commit/fsync pair that remains here is the C runtime's, not the Win32 API's.
//
// NOT IN THE SIMULATION DIGEST: Include/GameClient is not a digested root, though a digested file
// (Common/UserPreferences.cpp) includes it. It does no simulation work, and its Win32 arm has not
// been compiled yet (review of b101653df), so a fix to it after the first MSVC build must not move
// the sourceID and force another atomic four-platform ship. It lived in Include/Common until then.
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

// GeneralsX: A read that fails with ENOENT is a missing file
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

// GeneralsX: A NEW temp file beside finalPath, "<final>.<pid>.<n>.tmp", made
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

// GeneralsX: Copy srcPath over finalPath without ever
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
	//
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
