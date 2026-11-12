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

// Loads and unloads binkw32.dll at runtime; the Bink functions declared in bink.h forward to its exports.
// Loading explicitly, rather than importing, lets the library come from a working directory set on the command line.
// With no module loaded every Bink function returns a neutral value, so a missing dll only disables video.
// Not thread safe: every load needs a paired unload, from the main thread, while no Bink function is in flight.

class BinkLoader
{
public:

	// Returns whether binkw32.dll is loaded
	static bool isLoaded();

	// Returns whether binkw32.dll was attempted to be loaded but failed
	static bool isFailed();

	// Returns the system error code of the failed load attempt
	static unsigned long getLastError();

	static bool load();
	static void unload();
};
