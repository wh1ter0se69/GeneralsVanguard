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

#pragma once

#include "always.h"

#include "win.h"
#ifdef _WIN32
#include <imagehlp.h> // Must be included after Windows.h
#endif
#include <set>
#ifdef RTS_ENABLE_CRASHDUMP
#include "DbgHelpLoader_minidump.h"
#endif

#include "mutex.h"
#include "SystemAllocator.h"

// This static class can load, unload and use dbghelp.dll. Is thread-safe.
// Internally it must not use new and delete because it can be created during game memory initialization.

#ifdef _WIN32

class DbgHelpLoader
{
private:

	static DbgHelpLoader* Inst; // Is singleton class
	static CriticalSectionClass CriticalSection; // Required because dbg help is not thread safe for the most part

	DbgHelpLoader();
	~DbgHelpLoader();

public:

	// Returns whether dbghelp.dll is loaded
	static bool isLoaded();

	// Returns whether dbghelp.dll is loaded from the system directory
	static bool isLoadedFromSystem();

	// Returns whether dbghelp.dll was attempted to be loaded but failed
	static bool isFailed();

	// Every call to load needs a paired call to unload, no matter if the load was successful
	static bool load();
	static void unload();

	static BOOL WINAPI symInitialize(
		HANDLE hProcess,
		LPSTR UserSearchPath,
		BOOL fInvadeProcess);

	static BOOL WINAPI symCleanup(
		HANDLE hProcess);

	// GeneralsX: Address parameters must be pointer-width, not DWORD.
	//
	// dbghelp.h remaps every one of these to its ...64 form under _WIN64 - SymGetModuleBase becomes
	// SymGetModuleBase64, PIMAGEHLP_SYMBOL becomes PIMAGEHLP_SYMBOL64, STACKFRAME becomes STACKFRAME64
	// - and the 64-bit forms take DWORD64 addresses. Declaring them with DWORD made the wrappers below
	// incompatible with PFUNCTION_TABLE_ACCESS_ROUTINE64 / PGET_MODULE_BASE_ROUTINE64 at x64, which is
	// where StackDump.cpp's "cannot convert argument 7" C2664s came from. DWORD_PTR is DWORD at 32-bit
	// and DWORD64 at 64-bit, so both architectures get the exact type dbghelp expects and the 32-bit
	// build is bit-for-bit unchanged.
	static BOOL WINAPI symLoadModule(
		HANDLE hProcess,
		HANDLE hFile,
		LPSTR ImageName,
		LPSTR ModuleName,
		DWORD_PTR BaseOfDll,
		DWORD SizeOfDll);

	static DWORD_PTR WINAPI symGetModuleBase(
		HANDLE hProcess,
		DWORD_PTR dwAddr);

	static BOOL WINAPI symUnloadModule(
		HANDLE hProcess,
		DWORD_PTR BaseOfDll);

	static BOOL WINAPI symGetSymFromAddr(
		HANDLE hProcess,
		DWORD_PTR Address,
		PDWORD_PTR Displacement,
		PIMAGEHLP_SYMBOL Symbol);

	static BOOL WINAPI symGetLineFromAddr(
		HANDLE hProcess,
		DWORD_PTR dwAddr,
		PDWORD pdwDisplacement,
		PIMAGEHLP_LINE Line);

	static DWORD WINAPI symSetOptions(
		DWORD SymOptions);

	static LPVOID WINAPI symFunctionTableAccess(
		HANDLE hProcess,
		DWORD_PTR AddrBase);

	static BOOL WINAPI stackWalk(
		DWORD MachineType,
		HANDLE hProcess,
		HANDLE hThread,
		LPSTACKFRAME StackFrame,
		LPVOID ContextRecord,
		PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
		PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine,
		PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
		PTRANSLATE_ADDRESS_ROUTINE TranslateAddress);

#ifdef RTS_ENABLE_CRASHDUMP
	static BOOL WINAPI miniDumpWriteDump(
		HANDLE hProcess,
		DWORD ProcessId,
		HANDLE hFile,
		MINIDUMP_TYPE DumpType,
		PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
		PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
		PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
#endif

private:

	static void freeResources();

	typedef BOOL (WINAPI *SymInitialize_t) (
		HANDLE hProcess,
		LPSTR UserSearchPath,
		BOOL fInvadeProcess);

	typedef BOOL (WINAPI *SymCleanup_t) (
		HANDLE hProcess);

	typedef BOOL (WINAPI *SymLoadModule_t) (
		HANDLE hProcess,
		HANDLE hFile,
		LPSTR ImageName,
		LPSTR ModuleName,
		DWORD_PTR BaseOfDll,
		DWORD SizeOfDll);

	typedef DWORD_PTR (WINAPI *SymGetModuleBase_t) (
		HANDLE hProcess,
		DWORD_PTR dwAddr);

	typedef BOOL (WINAPI *SymUnloadModule_t) (
		HANDLE hProcess,
		DWORD_PTR BaseOfDll);

	typedef BOOL (WINAPI *SymGetSymFromAddr_t) (
		HANDLE hProcess,
		DWORD_PTR Address,
		PDWORD_PTR Displacement,
		PIMAGEHLP_SYMBOL Symbol);

	typedef BOOL (WINAPI* SymGetLineFromAddr_t) (
		HANDLE hProcess,
		DWORD_PTR dwAddr,
		PDWORD pdwDisplacement,
		PIMAGEHLP_LINE Line);

	typedef DWORD (WINAPI *SymSetOptions_t) (
		DWORD SymOptions);

	typedef LPVOID (WINAPI *SymFunctionTableAccess_t) (
		HANDLE hProcess,
		DWORD_PTR AddrBase);

	typedef BOOL (WINAPI *StackWalk_t) (
		DWORD MachineType,
		HANDLE hProcess,
		HANDLE hThread,
		LPSTACKFRAME StackFrame,
		LPVOID ContextRecord,
		PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
		PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine,
		PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
		PTRANSLATE_ADDRESS_ROUTINE TranslateAddress);

#ifdef RTS_ENABLE_CRASHDUMP
	typedef BOOL(WINAPI* MiniDumpWriteDump_t)(
		HANDLE hProcess,
		DWORD ProcessId,
		HANDLE hFile,
		MINIDUMP_TYPE DumpType,
		PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
		PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
		PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
#endif

	SymInitialize_t m_symInitialize;
	SymCleanup_t m_symCleanup;
	SymLoadModule_t m_symLoadModule;
	SymUnloadModule_t m_symUnloadModule;
	SymGetModuleBase_t m_symGetModuleBase;
	SymGetSymFromAddr_t m_symGetSymFromAddr;
	SymGetLineFromAddr_t m_symGetLineFromAddr;
	SymSetOptions_t m_symSetOptions;
	SymFunctionTableAccess_t m_symFunctionTableAccess;
	StackWalk_t m_stackWalk;
#ifdef RTS_ENABLE_CRASHDUMP
	MiniDumpWriteDump_t m_miniDumpWriteDump;
#endif

	typedef std::set<HANDLE, std::less<HANDLE>, stl::system_allocator<HANDLE>/**/> Processes;

	Processes m_initializedProcesses;
	HMODULE m_dllModule;
	int m_referenceCount;
	bool m_failed;
	bool m_loadedFromSystem;
};

#endif // _WIN32
