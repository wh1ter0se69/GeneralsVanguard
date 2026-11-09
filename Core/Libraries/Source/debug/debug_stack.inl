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

// Used for dynamically linking to dbghelp.dll functions.

// GeneralsX @bugfix Claude 27/07/2026 Address parameters are DWORD_PTR, not DWORD.
// imagehlp.h remaps StackWalk/SymFunctionTableAccess/SymGetModuleBase/SymGetSymFromAddr/
// SymGetLineFromAddr and the STACKFRAME/IMAGEHLP_* structs to their ...64 forms under _WIN64, and
// those take DWORD64 addresses. Leaving DWORD here made the generated function-pointer types
// incompatible with PFUNCTION_TABLE_ACCESS_ROUTINE64 / PGET_MODULE_BASE_ROUTINE64 at the
// gDbg._StackWalk() call. DWORD_PTR is DWORD at 32-bit, so nothing changes there.

// keep this always as first entry
DBGHELP(SymInitialize,
        BOOL,
        (HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess))

DBGHELP(SymGetOptions,
        DWORD,
        ())

DBGHELP(SymSetOptions,
        DWORD,
        (DWORD SymOptions))

DBGHELP(StackWalk,
        BOOL,
        (DWORD MachineType, HANDLE hProcess, HANDLE hThread, LPSTACKFRAME StackFrame,
        LPVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
        PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine,
        PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
        PTRANSLATE_ADDRESS_ROUTINE TranslateAddress))

DBGHELP(SymFunctionTableAccess,
        LPVOID,
        (HANDLE hProcess, DWORD_PTR AddrBase))

DBGHELP(SymGetModuleBase,
        DWORD_PTR,
        (HANDLE hProcess, DWORD_PTR dwAddr))

DBGHELP(SymGetSymFromAddr,
        BOOL,
        (HANDLE hProcess, DWORD_PTR Address, PDWORD_PTR Displacement,
        PIMAGEHLP_SYMBOL Symbol))

DBGHELP(SymGetLineFromAddr,
        BOOL,
        (HANDLE hProcess, DWORD_PTR dwAddr, PDWORD pdwDisplacement,
        PIMAGEHLP_LINE Line))

// keep this always as last entry
DBGHELP(SymCleanup,
        BOOL,
        (HANDLE hProcess))
