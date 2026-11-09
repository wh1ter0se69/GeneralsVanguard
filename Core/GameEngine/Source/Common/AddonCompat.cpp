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

#include "PreRTS.h"

#include "Common/AddonCompat.h"
#include "Common/FileSystem.h"

namespace addon
{
Bool HasFullviewportDat()
{
	Char value = '0';
	if (File* file = TheFileSystem->openFile("GenTool/fullviewport.dat", File::READ | File::BINARY))
	{
		file->read(&value, 1);
		// GeneralsX @bugfix The File handed back by openFile was never released. File derives from
		// MemoryPoolObject via MEMORY_POOL_GLUE_ABC, so its destructor is not publicly reachable and
		// letting the pointer go out of scope frees nothing; close() is the only release path (the
		// file systems hand out these objects with deleteOnClose set, so close() also deletes them).
		// Every call therefore leaked one File object plus the OS handle / RAM buffer behind it, once
		// per call site (GlobalData and GlobalLanguage), whenever GenTool/fullviewport.dat exists.
		file->close(); // NOTE: deletes 'file' - do not touch it after this point
	}
	return value != '0';
}

} // namespace addon
