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

#pragma once

// Loads and unloads mss32.dll at runtime; the Miles functions declared in mss/mss.h forward to its exports.
// Loading explicitly, rather than importing, lets the library come from a working directory set on the command line.
// A missing export is not an error: unresolved functions return a neutral value, so a missing dll only disables audio.
// Not thread safe: pair every load with an unload, on the main thread, outside AIL_startup..AIL_shutdown.

class MilesLoader
{
public:

	// Returns whether mss32.dll is loaded
	static bool isLoaded();

	// Returns whether mss32.dll was attempted to be loaded but failed
	static bool isFailed();

	// Returns the system error code of the failed load attempt
	static unsigned long getLastError();

	static bool load();
	static void unload();
};
