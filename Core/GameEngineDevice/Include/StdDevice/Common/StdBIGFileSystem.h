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

//////// StdBIGFileSystem.h ///////////////////////////
// Stephan Vedder, April 2025
/////////////////////////////////////////////////////////////

#pragma once

#include "Common/ArchiveFileSystem.h"

class StdBIGFileSystem : public ArchiveFileSystem
{
public:
	StdBIGFileSystem();
	virtual ~StdBIGFileSystem() override;

	virtual void init() override;
	virtual void update() override;
	virtual void reset() override;
	virtual void postProcessLoad() override;

	// ArchiveFile operations
	virtual void closeAllArchiveFiles() override;											///< Close all Archivefiles currently open

	// File operations
	virtual ArchiveFile * openArchiveFile(const Char *filename) override;
	virtual void closeArchiveFile(const Char *filename) override;
	virtual void closeAllFiles() override;															///< Close all files associated with ArchiveFiles

	virtual Bool loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite = FALSE) override;
	// GeneralsX @bugfix Claude 25/09/2026 The virtual above is reached only from ArchiveFileSystem::loadMods
	// and keeps retail's recursive walk of a -mod folder. Every game-folder scan in this file passes
	// searchSubdirectories=FALSE: see the comment on the definition.
	Bool loadBigFilesFromDirectory(AsciiString dir, AsciiString fileMask, Bool overwrite, Bool searchSubdirectories);
	virtual Bool promoteIfAlreadyMounted(const Char *filename) override;
	virtual Bool isModArchive(const Char *archiveName) const override;
protected:

	// GeneralsX @feature Claude 08/08/2026 Community-patch overlay directory.
	/// Mount every .big in the overlay directory with the precedence its FILENAME would
	/// have given it inside the game folder. See StdBIGFileSystem.cpp for why neither
	/// overwrite=FALSE nor overwrite=TRUE can express that.
	void loadPatchOverlay(const AsciiString& primaryAssetsDirectory);

	/// loadIntoDirectoryTree, but inserting at a filename-ordered position in each
	/// multimap range instead of at the front (overwrite=TRUE) or the back (FALSE).
	void loadIntoDirectoryTreeAtPriority(ArchiveFile *archiveFile, const AsciiString& priorityName);

	/// Absolute, canonical, no trailing separator. Empty when no overlay is configured or
	/// the configured one does not exist - in which case nothing below it ever runs.
	AsciiString m_patchOverlayDirectory;
};
