/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/WorkingDirectory.h"

namespace rts
{

Bool WorkingDirectory::s_hasSetWorkingDirectory = FALSE;
Char WorkingDirectory::s_startupWorkingDirectory[_MAX_PATH] = "";

struct WorkingDirectoryInitializer
{
	WorkingDirectoryInitializer()
	{
		WorkingDirectory::saveStartupWorkingDirectory();
	}
};

// Request capture at startup, even if no setter is called.
static WorkingDirectoryInitializer s_workingDirectoryInitializer;

Bool WorkingDirectory::saveStartupWorkingDirectory()
{
	// An earlier static constructor may call a setter before our initializer runs.
	// Cache the first capture, including failure, without logging or allocating.
	// GeneralsX: Off Windows this is the getcwd() shim in
	// CompatLib/Include/file_compat.h, which returns the same length-or-zero contract.
	static const DWORD len = ::GetCurrentDirectory(ARRAY_SIZE(s_startupWorkingDirectory), s_startupWorkingDirectory);
	return len > 0 && len < ARRAY_SIZE(s_startupWorkingDirectory);
}

Bool WorkingDirectory::setWorkingDirectory(const char *path)
{
	if (path == nullptr || path[0] == '\0')
	{
		DEBUG_LOG(("Cannot set an empty working directory"));
		return FALSE;
	}

	if (::SetCurrentDirectory(path) == 0)
	{
		// GeneralsX: GetLastError() has no shim off Windows.
#ifdef _WIN32
		DEBUG_LOG(("Failed to set working directory to '%s' (error %d)", path, GetLastError()));
#else
		DEBUG_LOG(("Failed to set working directory to '%s'", path));
#endif
		return FALSE;
	}

	s_hasSetWorkingDirectory = TRUE;
	return TRUE;
}

Bool WorkingDirectory::setStartupWorkingDirectory()
{
	if (!saveStartupWorkingDirectory())
	{
		DEBUG_LOG(("Startup working directory is unavailable"));
		return FALSE;
	}
	return setWorkingDirectory(s_startupWorkingDirectory);
}

Bool WorkingDirectory::setExecutableWorkingDirectory()
{
	saveStartupWorkingDirectory();

#ifdef _WIN32
	Char buffer[_MAX_PATH];
	const DWORD len = GetModuleFileName(nullptr, buffer, ARRAY_SIZE(buffer));
	if (len == 0)
	{
		DEBUG_LOG(("Failed to get executable path for working directory (error %d)", GetLastError()));
		return FALSE;
	}
	if (len >= ARRAY_SIZE(buffer))
	{
		DEBUG_LOG(("Executable path exceeds the working directory path buffer"));
		return FALSE;
	}

	Char *pEnd = strrchr(buffer, '\\');
	if (pEnd == nullptr)
	{
		DEBUG_LOG(("Executable path has no directory: '%s'", buffer));
		return FALSE;
	}
	// TheSuperHackers @bugfix For "C:\game.exe", retain "C:\" as the directory.
	// Removing the backslash leaves "C:", which refers to that drive's current directory.
	pEnd[1] = '\0';

	return setWorkingDirectory(buffer);
#else
	// GeneralsX: Deliberately not implemented off Windows.
	//
	// Upstream's body chdir()s to the executable's own directory, which on Windows is also the
	// data root. It is not on the platforms this fork targets: the macOS bundle puts the binary
	// in <app>/Contents/Resources/bin while the data root is chosen elsewhere (see
	// Core/GameEngine/Source/Common/System/GameMemoryInit.cpp), the Linux peers are launched
	// from $VPS_GAME by the harness, and iOS chdir()s inside SDL3Main. Doing it here would break
	// every cwd-relative data path, and dataID is cwd-relative, so it would refuse every join
	// with DATA_DIFFERS.
	//
	// It would have been accidentally inert today - GetModuleFileName's POSIX shim returns a
	// path with no backslash in it, so the strrchr would fail - but that is luck, not a design.
	// Off Windows the default stays "keep the working directory the OS gave us"; -setCwd and
	// -useCwd still work everywhere.
	DEBUG_LOG(("Executable working directory is not applied on this platform"));
	return FALSE;
#endif
}

Bool WorkingDirectory::setCustomWorkingDirectory(const char *path)
{
	saveStartupWorkingDirectory();
	return setWorkingDirectory(path);
}

Bool WorkingDirectory::hasSetWorkingDirectory()
{
	return s_hasSetWorkingDirectory;
}

} // namespace rts
