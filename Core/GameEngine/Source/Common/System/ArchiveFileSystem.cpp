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

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   Generals
//
// Module:    Game Engine Common
//
// File name: ArchiveFileSystem.cpp
//
// Created:   11/26/01 TR
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/AsciiString.h"
#include "Common/PerfTimer.h"


//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

ArchiveFileSystem *TheArchiveFileSystem = nullptr;


//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------

//------------------------------------------------------
// ArchivedFileInfo
//------------------------------------------------------
ArchiveFileSystem::ArchiveFileSystem()
{
}

ArchiveFileSystem::~ArchiveFileSystem()
{
	ArchiveFileMap::iterator iter = m_archiveFileMap.begin();
	while (iter != m_archiveFileMap.end()) {
		ArchiveFile *file = iter->second;
		delete file;
		iter++;
	}
}

void ArchiveFileSystem::loadIntoDirectoryTree(ArchiveFile *archiveFile, Bool overwrite)
{

	FilenameList filenameList;

	archiveFile->getFileListInDirectory("", "", "*", filenameList, TRUE);

	FilenameListIter it = filenameList.begin();

	while (it != filenameList.end())
	{
		ArchivedDirectoryInfo *dirInfo = &m_rootDirectory;

		AsciiString path;
		AsciiString token;
		AsciiString tokenizer = *it;
		tokenizer.toLower();
		tokenizer.nextToken(&token, "\\/");

		// GeneralsX: TheSuperHackers #3374 (3f3ed9fc8): walk directories until the path is used up;
		// the dot-guessing condition misfiled dotless file names as directories (tsh batch2 #53).
		while (!tokenizer.isEmpty())
		{
			path.concat(token);
			path.concat('\\');

			ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
			if (tempiter == dirInfo->m_directories.end())
			{
				dirInfo = &(dirInfo->m_directories[token]);
				dirInfo->m_path = path;
				dirInfo->m_directoryName = token;
			}
			else
			{
				dirInfo = &tempiter->second;
			}

			tokenizer.nextToken(&token, "\\/");
		}

		ArchivedFileLocationMap::iterator fileIt;
		if (overwrite)
		{
			// When overwriting, try place the new value at the beginning of the key list.
			fileIt = dirInfo->m_files.find(token);
		}
		else
		{
			// Append to the end of the key list.
			fileIt = dirInfo->m_files.end();
		}

		dirInfo->m_files.insert(fileIt, std::make_pair(token, archiveFile));

#if defined(DEBUG_LOGGING) && ENABLE_FILESYSTEM_LOGGING
		{
			const stl::const_range<ArchivedFileLocationMap> range = stl::get_range(dirInfo->m_files, token, 0);
			if (range.distance() >= 2)
			{
				ArchivedFileLocationMap::const_iterator rangeIt0;
				ArchivedFileLocationMap::const_iterator rangeIt1;

				if (overwrite)
				{
					rangeIt0 = range.begin;
					rangeIt1 = std::next(rangeIt0);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwriting same file in %s",
						it->str(),
						rangeIt0->second->getName().str(),
						rangeIt1->second->getName().str()
					));
				}
				else
				{
					rangeIt1 = std::prev(range.end);
					rangeIt0 = std::prev(rangeIt1);

					DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s, overwritten by same file in %s",
						it->str(),
						rangeIt1->second->getName().str(),
						rangeIt0->second->getName().str()
					));
				}
			}
			else
			{
				DEBUG_LOG(("ArchiveFileSystem::loadIntoDirectoryTree - adding file %s, archived in %s", it->str(), archiveFile->getName().str()));
			}
		}
#endif

		it++;
	}
}

void ArchiveFileSystem::loadMods()
{
	// GeneralsX: Same rule as loadBigFilesFromDirectory's guard: a -mod
	// BIG the asset scan already mounted is promoted, not opened and inserted a second time.
	if (TheGlobalData->m_modBIG.isNotEmpty() && !promoteIfAlreadyMounted(TheGlobalData->m_modBIG.str()))
	{
		ArchiveFile *archiveFile = openArchiveFile(TheGlobalData->m_modBIG.str());

		if (archiveFile != nullptr) {
			DEBUG_LOG(("ArchiveFileSystem::loadMods - loading %s into the directory tree.", TheGlobalData->m_modBIG.str()));
			loadIntoDirectoryTree(archiveFile, TRUE);
			m_archiveFileMap[TheGlobalData->m_modBIG] = archiveFile;
			DEBUG_LOG(("ArchiveFileSystem::loadMods - %s inserted into the archive file map.", TheGlobalData->m_modBIG.str()));
		}
		else
		{
			DEBUG_LOG(("ArchiveFileSystem::loadMods - could not openArchiveFile(%s)", TheGlobalData->m_modBIG.str()));
		}
	}

	if (TheGlobalData->m_modDir.isNotEmpty())
	{
		MAYBE_UNUSED Bool ret = loadBigFilesFromDirectory(TheGlobalData->m_modDir, "*.big", TRUE);
		(void)ret;
		DEBUG_ASSERTLOG(ret, ("loadBigFilesFromDirectory(%s) returned FALSE!", TheGlobalData->m_modDir.str()));
	}
}

// GeneralsX: Join-time SimID compatibility check - tier2 warn signal.
// The map is keyed by the RAW insertion name, not by basename - loadMods inserts
// TheGlobalData->m_modBIG verbatim above and loadBigFilesFromDirectory inserts
// directory-prefixed names - so iterating the map in its own order and hashing
// basenames would leave the ORDER path-dependent even though the VALUES are not. Two
// installs holding the same archives under different roots would then produce
// different fingerprints and warn forever. Collect, sort, then hash.
//
// Basename-only and lowercased so an install path can never itself cause a mismatch.
// Both separators are stripped: '/' for Apple, '\\' for a future Windows build and for
// the archive-internal form. Sizes are deliberately not hashed - that would make a
// re-packaged iOS archive a permanent difference.
UnsignedInt ArchiveFileSystem::computeMountedArchiveFingerprint() const
{
	UnsignedInt hash = 2166136261u;
	try
	{
		std::vector<std::string> names;
		names.reserve(m_archiveFileMap.size());

		for (ArchiveFileMap::const_iterator it = m_archiveFileMap.begin(); it != m_archiveFileMap.end(); ++it)
		{
			std::string s(it->first.str() ? it->first.str() : "");
			const size_t slash = s.find_last_of("/\\");
			if (slash != std::string::npos)
				s = s.substr(slash + 1);
			for (size_t i = 0; i < s.size(); ++i)
				s[i] = (char)tolower((unsigned char)s[i]);
			names.push_back(s);
		}

		std::sort(names.begin(), names.end());

		// GeneralsX: Print exactly what gets hashed.
		//
		// assetID is one of the two tier2 SimID fields, and when it disagrees between two
		// peers the value alone says nothing about WHICH archive is responsible. The engine's
		// own "inserted into the archive file map" trace is a DEBUG_LOG and is therefore
		// ((void)0) in every shipping preset, so in a Release build there was no way to see
		// the mounted set at all. fprintf(stderr) matches the release-visible pattern used by
		// the SimID and INI traces.
		fprintf(stderr, "[ARCHIVES] mounted=%u\n", (UnsignedInt)names.size());
		for (size_t n = 0; n < names.size(); ++n)
			fprintf(stderr, "[ARCHIVES]   %s\n", names[n].c_str());
		// Full map keys too: the basename is what gets hashed, but only the full path shows
		// whether a repeated basename is two different archives or the SAME file mounted twice.
		for (ArchiveFileMap::const_iterator it = m_archiveFileMap.begin(); it != m_archiveFileMap.end(); ++it)
			fprintf(stderr, "[ARCHPATH] %s\n", it->first.str() ? it->first.str() : "(null)");
		fflush(stderr);

		for (size_t n = 0; n < names.size(); ++n)
		{
			const std::string &s = names[n];
			for (size_t i = 0; i < s.size(); ++i)
			{
				hash ^= (unsigned char)s[i];
				hash *= 16777619u;
			}
			hash ^= (unsigned char)'\n';
			hash *= 16777619u;
		}
	}
	catch (...)
	{
		// Must not throw: the caller runs inside GameEngine::init's try block, and any
		// escape would reach catch (...) and abort startup.
		return 0u;
	}
	return hash;
}

Bool ArchiveFileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename);

	if (!result.valid())
		return false;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	return range.valid();
}

ArchivedDirectoryInfo* ArchiveFileSystem::friend_getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result = getArchivedDirectoryInfo(directory);

	return result.dirInfo;
}

ArchiveFileSystem::ArchivedDirectoryInfoResult ArchiveFileSystem::getArchivedDirectoryInfo(const Char* directory)
{
	ArchivedDirectoryInfoResult result;
	ArchivedDirectoryInfo* dirInfo = &m_rootDirectory;

	AsciiString token;
	AsciiString tokenizer = directory;
	tokenizer.toLower();
	tokenizer.nextToken(&token, "\\/");

	// GeneralsX: TheSuperHackers #3374 (3f3ed9fc8): walk directories until the path is used up;
	// the dot-guessing condition misfiled dotless file names as directories (tsh batch2 #53).
	while (!tokenizer.isEmpty())
	{
		ArchivedDirectoryInfoMap::iterator tempiter = dirInfo->m_directories.find(token);
		if (tempiter != dirInfo->m_directories.end())
		{
			dirInfo = &tempiter->second;
			tokenizer.nextToken(&token, "\\/");
		}
		else
		{
			// the directory doesn't exist
			result.dirInfo = nullptr;
			result.lastToken = AsciiString::TheEmptyString;
			return result;
		}
	}

	result.dirInfo = dirInfo;
	result.lastToken = token;
	return result;
}

File * ArchiveFileSystem::openFile(const Char *filename, Int access, FileInstance instance)
{
	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return nullptr;

	return archive->openFile(filename, access);
}

Bool ArchiveFileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance) const
{
	if (fileInfo == nullptr) {
		return FALSE;
	}

	if (filename.isEmpty()) {
		return FALSE;
	}

	ArchiveFile* archive = getArchiveFile(filename, instance);

	if (archive == nullptr)
		return FALSE;

	return archive->getFileInfo(filename, fileInfo);
}

ArchiveFile* ArchiveFileSystem::getArchiveFile(const AsciiString& filename, FileInstance instance) const
{
	ArchivedDirectoryInfoResult result = const_cast<ArchiveFileSystem*>(this)->getArchivedDirectoryInfo(filename.str());

	if (!result.valid())
		return nullptr;

	stl::const_range<ArchivedFileLocationMap> range = stl::get_range(result.dirInfo->m_files, result.lastToken, instance);

	if (!range.valid())
		return nullptr;
	
	return range.get()->second;
}

void ArchiveFileSystem::getFileListInDirectory(const AsciiString& currentDirectory, const AsciiString& originalDirectory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	ArchiveFileMap::const_iterator it = m_archiveFileMap.begin();
	while (it != m_archiveFileMap.end()) {
		it->second->getFileListInDirectory(currentDirectory, originalDirectory, searchName, filenameList, searchSubdirectories);
		it++;
	}
}
