/*
 * GeneralsX @bugfix Claude 27/07/2026 Architecture-neutral access to the Windows CONTEXT record.
 *
 * The crash reporters (WWLib/Except.cpp, <game>/GameEngine/Source/Common/System/StackDump.cpp and
 * Libraries/Source/debug/debug_*.cpp) were written for 32-bit x86 and hardcode three things that
 * do not exist at x64:
 *
 *   1. CONTEXT::Eip / Esp / Ebp. The x64 CONTEXT calls them Rip / Rsp / Rbp, so every reference
 *      was "error C2039: 'Eip': is not a member of '_CONTEXT'".
 *   2. MSVC inline assembly, to read those registers at the current instruction. /Zi-era __asm is
 *      simply not supported by the x64 compiler ("error C4235: nonstandard extension used: '__asm'
 *      keyword not supported on this architecture"), and the surrounding token soup then produces
 *      a cascade of C2065s.
 *   3. IMAGE_FILE_MACHINE_I386 as the StackWalk machine type.
 *
 * RtlCaptureContext solves (2) on every Windows architecture without a line of assembly - it is
 * exported from kernel32 and declared in winnt.h for MSVC and mingw-w64 alike - so this header
 * uses it for x86 as well and the games end up with a single code path instead of three.
 *
 * KNOWN LIMITATION at x64: StackWalk64 cannot walk an x64 stack from a frame pointer, because x64
 * code is compiled without one; it needs the CONTEXT record and the unwind tables. Call sites that
 * currently hand StackWalk a null ContextRecord should pass GX_STACKWALK_CONTEXT(&ctx) instead,
 * which is a no-op at x86 (preserving the original frame-pointer walk) and the real context at
 * x64. Where a call site has not been converted the walk degrades to producing no frames, which is
 * a diagnostic gap, never a crash.
 *
 * Windows-only by construction: everything below sits inside #ifdef _WIN32.
 */
#pragma once

#ifdef _WIN32

#include <windows.h>
#include <stddef.h>
#include <string.h>

/* Wide enough to hold a code address on the target: 4 bytes at x86, 8 at x64/arm64. Matches what
   STACKFRAME::AddrPC.Offset wants, since dbghelp.h remaps STACKFRAME to STACKFRAME64 under
   _WIN64 and widens Offset to DWORD64 with it. size_t rather than DWORD_PTR so the same spelling
   can appear in headers that non-Windows targets also parse. */
typedef size_t GXCtxReg;

#if defined(_M_X64) || defined(_M_AMD64)
	#define GX_CTX_PC(ctxptr) ((ctxptr)->Rip)
	#define GX_CTX_SP(ctxptr) ((ctxptr)->Rsp)
	#define GX_CTX_FP(ctxptr) ((ctxptr)->Rbp)
	#define GX_IMAGE_FILE_MACHINE_NATIVE IMAGE_FILE_MACHINE_AMD64
#elif defined(_M_ARM64)
	#define GX_CTX_PC(ctxptr) ((ctxptr)->Pc)
	#define GX_CTX_SP(ctxptr) ((ctxptr)->Sp)
	#define GX_CTX_FP(ctxptr) ((ctxptr)->Fp)
	#define GX_IMAGE_FILE_MACHINE_NATIVE IMAGE_FILE_MACHINE_ARM64
#else
	#define GX_CTX_PC(ctxptr) ((ctxptr)->Eip)
	#define GX_CTX_SP(ctxptr) ((ctxptr)->Esp)
	#define GX_CTX_FP(ctxptr) ((ctxptr)->Ebp)
	#define GX_IMAGE_FILE_MACHINE_NATIVE IMAGE_FILE_MACHINE_I386
#endif

/* StackWalk's ContextRecord argument. x86 walks the EBP chain and the original code deliberately
   passed null; x64 has no frame-pointer chain to walk and requires the record. */
#if defined(_M_IX86)
	#define GX_STACKWALK_CONTEXT(ctxptr) ((LPVOID)0)
#else
	#define GX_STACKWALK_CONTEXT(ctxptr) ((LPVOID)(ctxptr))
#endif

/*
 * Capture the calling thread's register state. ctx may be null if only the three values are
 * wanted. Deliberately NOT force-inlined: RtlCaptureContext records the state at its own call
 * site, so the program counter lands inside this helper. Every caller already walks with a
 * skip-frames count, and one extra frame is cheaper than reintroducing per-architecture assembly.
 */
static inline void GXCaptureContextRegisters(CONTEXT *ctx, GXCtxReg *pc, GXCtxReg *sp, GXCtxReg *fp)
{
	CONTEXT local;
	CONTEXT *use = (ctx != 0) ? ctx : &local;

	memset(use, 0, sizeof(CONTEXT));
	use->ContextFlags = CONTEXT_FULL;
	RtlCaptureContext(use);

	if (pc != 0) { *pc = (GXCtxReg)GX_CTX_PC(use); }
	if (sp != 0) { *sp = (GXCtxReg)GX_CTX_SP(use); }
	if (fp != 0) { *fp = (GXCtxReg)GX_CTX_FP(use); }
}

#endif /* _WIN32 */
