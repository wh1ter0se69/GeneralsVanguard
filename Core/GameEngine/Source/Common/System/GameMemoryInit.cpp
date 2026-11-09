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

// FILE: MemoryInit.cpp
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: MemoryInit.cpp
//
// Created:   Steven Johnson, August 2001
//
// Desc:      Memory manager
//
// ----------------------------------------------------------------------------
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// SYSTEM INCLUDES

// USER INCLUDES
#include "Lib/BaseType.h"
#include "Common/GameMemory.h"

struct PoolSizeRec
{
	const char* name;
	Int initial;
	Int overflow;
};

#if RTS_GENERALS
#include "GameMemoryInitDMA_Generals.inl"
#include "GameMemoryInitPools_Generals.inl"
#elif RTS_ZEROHOUR
#include "GameMemoryInitDMA_GeneralsMD.inl"
#include "GameMemoryInitPools_GeneralsMD.inl"
#endif

//-----------------------------------------------------------------------------
void userMemoryManagerGetDmaParms(Int *numSubPools, const PoolInitRec **pParms)
{
	*numSubPools = ARRAY_SIZE(DefaultDMA);
	*pParms = DefaultDMA;
}

//-----------------------------------------------------------------------------
void userMemoryAdjustPoolSize(const char *poolName, Int& initialAllocationCount, Int& overflowAllocationCount)
{
	if (initialAllocationCount > 0)
		return;

	for (const PoolSizeRec* p = PoolSizes; p->name != nullptr; ++p)
	{
		if (strcmp(p->name, poolName) == 0)
		{
			initialAllocationCount = p->initial;
			overflowAllocationCount = p->overflow;
			return;
		}
	}

	DEBUG_CRASH(("Initial size for pool %s not found -- you should add it to MemoryInit.cpp",poolName));
}

//-----------------------------------------------------------------------------
static Int roundUpMemBound(Int i)
{
	const int MEM_BOUND_ALIGNMENT = 4;

	if (i < MEM_BOUND_ALIGNMENT)
		return MEM_BOUND_ALIGNMENT;
	else
		return (i + (MEM_BOUND_ALIGNMENT-1)) & ~(MEM_BOUND_ALIGNMENT-1);
}

//-----------------------------------------------------------------------------
void userMemoryManagerInitPools()
{
	// note that we MUST use stdio stuff here, and not the normal game file system
	// (with bigfile support, etc), because that relies on memory pools, which
	// aren't yet initialized properly! so rely ONLY on straight stdio stuff here.
	// (not even AsciiString. thanks.)

	// since we're called prior to main, the cur dir might not be what
	// we expect. so do it the hard way.
	// GeneralsX @bugfix Claude 25/07/2026 buf was uninitialized and the result of
	// GetModuleFileName() was ignored. The Apple implementation (module_compat.cpp, via
	// _NSGetExecutablePath) leaves the buffer untouched when it fails, so the code below
	// would parse and concatenate onto stack garbage. Nothing else happens in this function,
	// so bailing out here only skips the optional override, exactly as a missing file does.
	// Note: no diagnostic here on purpose -- we run before DEBUG_INIT (see initMemoryManager
	// in GameMemory.cpp), so DEBUG_LOG/DEBUG_CRASH are not usable at this point.
	char buf[_MAX_PATH] = { 0 };
	::GetModuleFileName(nullptr, buf, sizeof(buf));
	if (buf[0] == 0)
		return;

	// GeneralsX @bugfix Claude 25/07/2026 this split only on '\\'. On macOS/iOS the executable
	// path is a POSIX path, so strrchr() returned null, the executable filename was never
	// stripped, and the Windows-style "\\Data\\INI\\MemoryPools.ini" was appended to the *file*
	// name. fopen() then failed silently, which made MemoryPools.ini inert on every Apple
	// build -- the one data-side knob for shrinking pools on a memory-constrained iPad, where
	// an undersized pool otherwise aborts with ERROR_OUT_OF_MEMORY. Split on whichever
	// separator comes last, mirroring getExecutableDirectory() in StdBIGFileSystem.cpp, and
	// append with the platform's own separator.
	char* pSlash = strrchr(buf, '/');
	char* pBackslash = strrchr(buf, '\\');
	char* pEnd = pSlash;
	if (pEnd == nullptr || (pBackslash != nullptr && pBackslash > pEnd))
	{
		pEnd = pBackslash;
	}
	if (pEnd != nullptr)
	{
		*pEnd = 0;
	}
#ifdef _WIN32
	strlcat(buf, "\\Data\\INI\\MemoryPools.ini", ARRAY_SIZE(buf));
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// GeneralsX @bugfix On iOS the game data does not sit beside the executable —
	// the packaging script rsyncs the whole install into <bundle>/GameData (see
	// scripts/build/ios/package-ios-zh.sh), and SDL3Main derives exactly that path.
	// Appending Data/INI directly to the executable directory therefore still
	// missed the file and left MemoryPools.ini inert on the one platform whose
	// memory limits make it matter.
	//
	// The working directory cannot be relied on here: this runs pre-main from
	// preMainInitMemoryManager, before SDL3Main chdir()s into the data directory.
	strlcat(buf, "/GameData/Data/INI/MemoryPools.ini", ARRAY_SIZE(buf));
#else
	strlcat(buf, "/Data/INI/MemoryPools.ini", ARRAY_SIZE(buf));
#endif

	FILE* fp = fopen(buf, "r");
	// GeneralsX @bugfix Claude 25/07/2026 the executable directory is not the install root
	// in every supported layout, so the separator fix above still left this file unreachable
	// for the bundled macOS build: bundle-macos-zh.sh puts the binary in
	// <app>/Contents/Resources/bin while the game data stays in the user's Zero Hour
	// directory, so <exeDir>/Data/INI never exists there and MemoryPools.ini remained inert.
	// That wrapper chdir()s into the data root before exec, so the process already starts
	// with the right working directory and a cwd-relative retry finds the file -- and cwd is
	// where every other Data/INI file is read from anyway. Only attempted after the
	// exe-relative open has already failed, so no layout that worked before changes, and a
	// miss here is as silent and harmless as a missing file (this runs before DEBUG_INIT,
	// see preMainInitMemoryManager in GameMemory.cpp, so we cannot log either way).
	if (fp == nullptr)
	{
		fp = fopen("Data/INI/MemoryPools.ini", "r");
	}
	if (fp)
	{
		char poolName[256];
		int initial, overflow;
		while (fgets(buf, _MAX_PATH, fp))
		{
			if (buf[0] == ';')
				continue;
			// GeneralsX @bugfix Claude 25/07/2026 unbounded "%s" wrote into poolName[256] from a
			// line of up to _MAX_PATH (1024) bytes, so any long token in MemoryPools.ini smashed
			// the stack. Bound the conversion to the buffer size (255 chars + terminator).
			if (sscanf(buf, "%255s %d %d", poolName, &initial, &overflow ) == 3)
			{
				for (PoolSizeRec* p = PoolSizes; p->name != nullptr; ++p)
				{
					if (stricmp(p->name, poolName) == 0)
					{
						// currently, these must be multiples of 4. so round up.
						p->initial = roundUpMemBound(initial);
						p->overflow = roundUpMemBound(overflow);
						break;	// from for-p
					}
				}
			}
		}
		fclose(fp);
	}
}

