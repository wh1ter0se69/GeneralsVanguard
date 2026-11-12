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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

///////// Win32LocalFileSystem.cpp /////////////////////////
// Bryan Cleveland, August 2002
////////////////////////////////////////////////////////////

#include <windows.h>
#include "Common/AsciiString.h"
#include "Common/GameMemory.h"
#include "Common/PerfTimer.h"
#include "Win32Device/Common/Win32LocalFileSystem.h"
#include "Win32Device/Common/Win32LocalFile.h"
#include <io.h>

// GeneralsX: Asset-root fallback for loose data files. Win32BIGFileSystem hands CNC_GENERALS_ZH_PATH here, so relative
// loose files resolve when cwd != install dir (a missed Data\Scripts\SkirmishScripts.scb causes a frame-0 desync).
// Consulted ONLY when the path is relative and the cwd-relative lookup already failed, so no working lookup changes.
static AsciiString s_win32AssetFallbackPath;

/// Returns assetRoot\filename, or an empty string when no fallback is possible.
static AsciiString buildAssetRootPath(const Char *filename)
{
	if (s_win32AssetFallbackPath.isEmpty() || filename == nullptr || *filename == '\0')
		return AsciiString::TheEmptyString;

	// Absolute paths must never be rewritten - only cwd-relative ones can be "missing".
	// "C:\..." and a leading slash are both absolute; "\\server\share" is a UNC path.
	if (filename[0] == '\\' || filename[0] == '/')
		return AsciiString::TheEmptyString;
	if (filename[1] == ':')
		return AsciiString::TheEmptyString;

	AsciiString candidate = s_win32AssetFallbackPath;
	const Int len = candidate.getLength();
	if (len > 0)
	{
		const char last = candidate.getCharAt(len - 1);
		if (last != '\\' && last != '/')
			candidate.concat('\\');
	}
	candidate.concat(filename);
	return candidate;
}

Win32LocalFileSystem::Win32LocalFileSystem() : LocalFileSystem()
{
}

Win32LocalFileSystem::~Win32LocalFileSystem() {
}

void Win32LocalFileSystem::setAssetRootPath(const AsciiString& path)
{
	s_win32AssetFallbackPath = path;
	DEBUG_LOG(("Win32LocalFileSystem::setAssetRootPath - asset fallback path set to '%s'", path.str()));
}

//DECLARE_PERF_TIMER(Win32LocalFileSystem_openFile)
File * Win32LocalFileSystem::openFile(const Char *filename, Int access, size_t bufferSize)
{
	//USE_PERF_TIMER(Win32LocalFileSystem_openFile)

	// sanity check
	if (strlen(filename) <= 0) {
		return nullptr;
	}

	if (access & File::WRITE) {
		// if opening the file for writing, we need to make sure the directory is there
		// before we try to create the file.
		AsciiString string;
		string = filename;
		AsciiString token;
		AsciiString dirName;
		string.nextToken(&token, "\\/");
		dirName = token;
		// GeneralsX: TheSuperHackers #3374 (3f3ed9fc8): walk directories until the path is used up;
		// the dot-guessing condition misfiled dotless file names as directories (tsh batch2 #53).
		while (!string.isEmpty()) {
			createDirectory(dirName);
			string.nextToken(&token, "\\/");
			dirName.concat('\\');
			dirName.concat(token);
		}
	}

	// TheSuperHackers @fix Mauller 21/04/2025 Create new file handle when necessary to prevent memory leak
	Win32LocalFile *file = newInstance( Win32LocalFile );

	if (file->open(filename, access, bufferSize) == FALSE) {
		deleteInstance(file);
		file = nullptr;

		// GeneralsX: Retry from the asset root. Reads only - a WRITE that
		// failed must NOT be silently redirected into the install directory, which on a Steam
		// install is both read-only by policy and the wrong place for user data.
		if (!(access & File::WRITE))
		{
			const AsciiString fallback = buildAssetRootPath(filename);
			if (fallback.isNotEmpty())
			{
				Win32LocalFile *fallbackFile = newInstance( Win32LocalFile );
				if (fallbackFile->open(fallback.str(), access, bufferSize) == FALSE) {
					deleteInstance(fallbackFile);
				} else {
					fallbackFile->deleteOnClose();
					file = fallbackFile;
				}
			}
		}
	} else {
		file->deleteOnClose();
	}

// this will also need to play nice with the STREAMING type that I added, if we ever enable this

// srj sez: this speeds up INI loading, but makes BIG files unusable.
// don't enable it without further tweaking.
//
// unless you like running really slowly.
//	if (!(access&File::WRITE)) {
//		// Return a ramfile.
//		RAMFile *ramFile = newInstance( RAMFile );
//		if (ramFile->open(file)) {
//			file->close(); // is deleteonclose, so should delete.
//			ramFile->deleteOnClose();
//			return ramFile;
//		}	else {
//			ramFile->close();
//			deleteInstance(ramFile);
//		}
//	}

	return file;
}

void Win32LocalFileSystem::update()
{
}

void Win32LocalFileSystem::init()
{
}

void Win32LocalFileSystem::reset()
{
}

//DECLARE_PERF_TIMER(Win32LocalFileSystem_doesFileExist)
Bool Win32LocalFileSystem::doesFileExist(const Char *filename) const
{
	//USE_PERF_TIMER(Win32LocalFileSystem_doesFileExist)
	if (_access(filename, 0) == 0) {
		return TRUE;
	}

	// GeneralsX: Same asset-root fallback as openFile. This must agree
	// with openFile or callers that probe before opening (the map cache is one) conclude a file
	// is absent that openFile would happily return.
	const AsciiString fallback = buildAssetRootPath(filename);
	if (fallback.isNotEmpty() && _access(fallback.str(), 0) == 0) {
		return TRUE;
	}
	return FALSE;
}

void Win32LocalFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList & filenameList, Bool searchSubdirectories) const
{
	HANDLE fileHandle = nullptr;
	WIN32_FIND_DATA findData;

	AsciiString asciisearch;
	asciisearch = originalDirectory;
	asciisearch.concat(currentDirectory);
	asciisearch.concat(searchName);

	Bool done = FALSE;

	fileHandle = FindFirstFile(asciisearch.str(), &findData);
	done = (fileHandle == INVALID_HANDLE_VALUE);

	while (!done)	{
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				(strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)) {
			// if we haven't already, add this filename to the list.
				// a stl set should only allow one copy of each filename
				AsciiString newFilename;
				newFilename = originalDirectory;
				newFilename.concat(currentDirectory);
				newFilename.concat(findData.cFileName);
				if (filenameList.find(newFilename) == filenameList.end()) {
					filenameList.insert(newFilename);
				}
		}

		done = (FindNextFile(fileHandle, &findData) == 0);
	}
	FindClose(fileHandle);

	if (searchSubdirectories) {
		AsciiString subdirsearch;
		subdirsearch = originalDirectory;
		subdirsearch.concat(currentDirectory);
		subdirsearch.concat("*.");
		fileHandle = FindFirstFile(subdirsearch.str(), &findData);
		done = fileHandle == INVALID_HANDLE_VALUE;

		while (!done) {
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
					(strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)) {

					AsciiString tempsearchstr;
					tempsearchstr.concat(currentDirectory);
					tempsearchstr.concat(findData.cFileName);
					tempsearchstr.concat('\\');

					// recursively add files in subdirectories if required.
					getFileListInDirectory(tempsearchstr, originalDirectory, searchName, filenameList, searchSubdirectories);
			}

			done = (FindNextFile(fileHandle, &findData) == 0);
		}

		FindClose(fileHandle);
	}

}

Bool Win32LocalFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo) const
{
	WIN32_FIND_DATA findData;
	HANDLE findHandle = nullptr;
	findHandle = FindFirstFile(filename.str(), &findData);

	if (findHandle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	fileInfo->timestampHigh = findData.ftLastWriteTime.dwHighDateTime;
	fileInfo->timestampLow = findData.ftLastWriteTime.dwLowDateTime;
	fileInfo->sizeHigh = findData.nFileSizeHigh;
	fileInfo->sizeLow = findData.nFileSizeLow;

	FindClose(findHandle);

	return TRUE;
}

Bool Win32LocalFileSystem::createDirectory(AsciiString directory)
{
	if ((!directory.isEmpty()) && (directory.getLength() < _MAX_DIR)) {
		return (CreateDirectory(directory.str(), nullptr) != 0);
	}
	return FALSE;
}

AsciiString Win32LocalFileSystem::normalizePath(const AsciiString& filePath) const
{
	DWORD retval = GetFullPathNameA(filePath.str(), 0, nullptr, nullptr);
	if (retval == 0)
	{
		DEBUG_LOG(("Unable to determine buffer size for normalized file path. Error=(%u).", GetLastError()));
		return AsciiString::TheEmptyString;
	}

	AsciiString normalizedFilePath;
	retval = GetFullPathNameA(filePath.str(), retval, normalizedFilePath.getBufferForRead(retval - 1), nullptr);
	if (retval == 0)
	{
		DEBUG_LOG(("Unable to normalize file path '%s'. Error=(%u).", filePath.str(), GetLastError()));
		return AsciiString::TheEmptyString;
	}

	return normalizedFilePath;
}
