/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

///////// Win32ReplaceFile.cpp /////////////////////////////
//
// GeneralsX @feature Claude 23/09/2026 The Win32 arm of gxCommitAndReplace (GameClient/GXReplaceFile.h):
// put a finished .tmp in place of the real file. MSVCRT's rename() refuses an existing destination,
// so this is MoveFileExA with MOVEFILE_REPLACE_EXISTING; MOVEFILE_WRITE_THROUGH returns only once the
// rename has reached the disk. The "A" form because the caller opened the .tmp with the same narrow
// path through fopen(); a wide conversion here could name a different file.
//
// Moved out of the shared header after the review of 5cdc742e1 (platform code stays in the platform
// layer). NOT compiled by MSVC on this branch yet - the first Windows build is the check.
////////////////////////////////////////////////////////////

#include <windows.h>
#include "GameClient/GXReplaceFile.h"

bool gxPlatformReplaceFile(const char *tmpPath, const char *finalPath)
{
	return MoveFileExA(tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
