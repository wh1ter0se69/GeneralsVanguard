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

// FILE: GXStderrLog.cpp //////////////////////////////////////////////////////
//
// GeneralsX @feature Claude 17/08/2026 See GXStderrLog.h for why this exists.
//
// Two platform mechanisms, one shared policy:
//
//   Darwin (macOS + iOS)  funopen() installs a write callback on the stderr
//                         FILE. fd 2 itself is left alone, so everything still
//                         reaches wherever stderr already pointed. The callback
//                         only queues; a pump thread writes (02/10/2026).
//
//   Windows               MSVC has no funopen()/fopencookie() - there is no
//                         supported way to hand the CRT a write callback for a
//                         FILE. The interception point that DOES exist is the
//                         descriptor: an anonymous pipe is dup2'd onto stderr's
//                         fd and a helper thread drains it, applying the same
//                         policy. This is strictly better than a plain
//                         freopen(): it keeps the filter and the cap, and
//                         because it replaces the DESCRIPTOR (and the
//                         STD_ERROR_HANDLE), it also captures writes made
//                         directly to the handle by code that never touches the
//                         CRT's stderr - DXVK's logger among them.
//
//   Linux                 funopen() is Darwin-only and fopencookie() is glibc-
//                         only; the Linux peer is the one platform whose stderr
//                         the harnesses already capture to a file, so it needs
//                         no sink and gets none.
//
///////////////////////////////////////////////////////////////////////////////

#include "GXStderrLog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>  // intptr_t, for _open_osfhandle
#else
#include <unistd.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <atomic>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>
#endif
#endif

// ---------------------------------------------------------------------------
// Policy - shared by every platform, and the part that must not be got wrong.
// ---------------------------------------------------------------------------

// A marathon session must not eat the disk. 8 MB matches what the iOS sink has
// shipped with since 07/08/2026.
static const size_t kLogCap = 8u * 1024u * 1024u;

// Dropped while under the cap: known per-frame / per-launch spam that would
// otherwise burn the whole budget before anything interesting happens.
// [INI] alone is a few thousand lines every single launch.
static const char *const kDropPrefixes[] = {
	"[GX-ISSUE144]",  // upstream font trace, per frame
	"[INI] ",         // INI loader trace
	"warn:  D3D8De",  // residual DXVK d3d8 per-call warn
};

// Still recorded AFTER the cap. The tail of a dying session is this file's
// whole reason to exist.
//
// [CRC] IS THE POINT OF THIS FILE. The iOS sink's past-cap allow-list was
// err:/ERROR/FATAL only, so a desync at 8 MB+ would have been silently dropped
// by the very sink that was supposed to record it. It is first in the list so
// that is hard to undo by accident.
static const char *const kKeepPastCapPrefixes[] = {
	"[CRC]",   // desync evidence: MISMATCH, SHORTFALL and the ok heartbeat
	"err:",    // DXVK
	"ERROR",
	"FATAL",   // includes Debug.cpp's "FATAL: release crash:" and the crash handlers
	"[GXRELAY]", // relay mode ON/OFF, once per socket init: the LAST one is the relay state, so a
	             // re-init late in a long session must not be dropped (Claude 19/09/2026)
	// GeneralsX @bugfix Claude 23/09/2026 INI.cpp's real errors ("[INI] ERROR: No files read from
	// directory ..." - the first-install symptom StdBIGFileSystem.cpp cites) share the dropped
	// "[INI] " prefix. A line on this list is never dropped, under the cap or past it.
	"[INI] ERROR",
};

static bool s_atLineStart = true;
static bool s_dropLine = false;
static bool s_keepPastCap = false;
static bool s_capMarked = false;
static size_t s_written = 0;
static bool s_installed = false;

// Linux has neither funopen() (Darwin-only) nor a reason to want one: its
// stderr is already captured to a file by every harness that runs it.
#if defined(_WIN32) || defined(__APPLE__)
#define GX_STDERR_SINK_SUPPORTED 1
#else
#define GX_STDERR_SINK_SUPPORTED 0
#endif

#ifdef _WIN32
static HANDLE s_logHandle = INVALID_HANDLE_VALUE;
static HANDLE s_pipeRead = INVALID_HANDLE_VALUE;
static HANDLE s_teeHandle = INVALID_HANDLE_VALUE;
static int s_stderrFd = -1;  // the descriptor the pipe was dup2'd onto
#else
static int s_logFd = -1;
#endif

/// Write straight to the log file. Never buffered: a crash must not be able to
/// strand the last few lines in userspace.
static int gxRawWrite(const char *buf, int len)
{
#ifdef _WIN32
	if (s_logHandle == INVALID_HANDLE_VALUE)
		return 0;
	DWORD wrote = 0;
	if (!WriteFile(s_logHandle, buf, (DWORD)len, &wrote, NULL))
		return 0;
	return (int)wrote;
#else
	if (s_logFd < 0)
		return 0;
	const ssize_t w = write(s_logFd, buf, (size_t)len);
	return (w > 0) ? (int)w : 0;
#endif
}

static bool gxHasPrefix(const char *buf, int len, const char *prefix)
{
	const size_t n = strlen(prefix);
	return (size_t)len >= n && memcmp(buf, prefix, n) == 0;
}

static bool gxMatchesAny(const char *buf, int len, const char *const *list, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		if (gxHasPrefix(buf, len, list[i]))
			return true;
	}
	return false;
}

void GXStderrLog_Consume(const char *buf, int len)
{
	if (buf == NULL || len <= 0)
		return;

	int i = 0;
	while (i < len)
	{
		// Classify once per line. The caller hands us chunks that begin on a
		// line boundary, so the prefix is always intact at this point; the
		// s_atLineStart / s_dropLine state carries the verdict across a chunk
		// boundary that falls in the middle of a line.
		if (s_atLineStart)
		{
			s_keepPastCap = gxMatchesAny(buf + i, len - i, kKeepPastCapPrefixes,
				sizeof(kKeepPastCapPrefixes) / sizeof(kKeepPastCapPrefixes[0]));
			// GeneralsX @bugfix Claude 23/09/2026 A keep-listed line beats a drop prefix ("[INI] ERROR").
			s_dropLine = !s_keepPastCap && gxMatchesAny(buf + i, len - i, kDropPrefixes,
				sizeof(kDropPrefixes) / sizeof(kDropPrefixes[0]));
			s_atLineStart = false;
		}

		const char *nl = (const char *)memchr(buf + i, '\n', (size_t)(len - i));
		const int seg = nl != NULL ? (int)(nl - (buf + i)) + 1 : (len - i);

		const bool pastCap = (s_written >= kLogCap);
		bool emit;
		if (pastCap)
		{
			if (!s_capMarked)
			{
				s_capMarked = true;
				static const char mark[] =
					"[gx] log capped: only error, [CRC] and [GXRELAY] lines are recorded from here\n";
				gxRawWrite(mark, (int)(sizeof(mark) - 1));
			}
			emit = s_keepPastCap;
		}
		else
		{
			emit = !s_dropLine;
		}

		if (emit)
		{
			const int w = gxRawWrite(buf + i, seg);
			if (w > 0 && !pastCap)
				s_written += (size_t)w;
		}

		i += seg;
		if (nl != NULL)
			s_atLineStart = true;
	}
}

// ---------------------------------------------------------------------------
// File creation and rotation - identical on every platform.
// ---------------------------------------------------------------------------

static const char *const kLogLeaf = "generals-stderr.log";
static const char *const kPrevLeaf = "generals-stderr-prev.log";

static bool gxBuildPath(char *out, size_t outSize, const char *dir, const char *leaf)
{
	const size_t dirLen = strlen(dir);
	const size_t leafLen = strlen(leaf);
	// +1 for a separator we may have to add, +1 for the terminator.
	if (dirLen + leafLen + 2 > outSize)
		return false;

	memcpy(out, dir, dirLen);
	size_t n = dirLen;
	if (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\')
	{
#ifdef _WIN32
		out[n++] = '\\';
#else
		out[n++] = '/';
#endif
	}
	memcpy(out + n, leaf, leafLen);
	out[n + leafLen] = '\0';
	return true;
}

/// Rotate the previous session's log aside and open a fresh one.
/// A session that dies leaves the prior log as the only evidence, so the
/// rotation matters as much as the log.
static bool gxOpenLogFile(const char *dir)
{
	char curPath[1024];
	char prevPath[1024];
	if (!gxBuildPath(curPath, sizeof(curPath), dir, kLogLeaf))
		return false;
	if (!gxBuildPath(prevPath, sizeof(prevPath), dir, kPrevLeaf))
		return false;

	remove(prevPath);
	rename(curPath, prevPath);

#ifdef _WIN32
	// FILE_SHARE_READ so the log can be tailed/copied while the game runs.
	s_logHandle = CreateFileA(curPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	return s_logHandle != INVALID_HANDLE_VALUE;
#else
	s_logFd = open(curPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	return s_logFd >= 0;
#endif
}

// ---------------------------------------------------------------------------
// Platform plumbing.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// GeneralsX @bugfix D · native-windows-1 13/10/2026 The tee is bounded (render-seam INTEGRATION.md residual R24).
// The pump used to WriteFile() the tee itself. A tee whose reader never drains - an ssh channel nobody reads, a
// stalled console, a parent that stopped reading its pipe - blocked the pump in that WriteFile; the 64 KB stderr pipe
// then filled and every fprintf(stderr) in the process blocked with it: the main thread, DXVK's logger, the native
// renderer's threads. Now the pump only copies into this ring and a tee thread of its own does the blocking write. A
// full ring drops the newest bytes and counts them (the macOS sink's rule: never block a printer on a reader that does
// not read); the pump writes "[gx] stderr tee dropped N bytes" into the log file when the count grows. The log file and
// the filter are unchanged; a draining tee gets every byte as before.
static const size_t kTeeRing = 1u << 20;
static char s_teeRing[kTeeRing];
static size_t s_teeHead = 0, s_teeTail = 0;          // monotonic byte counts under s_teeLock (tail - head = queued)
static unsigned long long s_teeDropped = 0;           // under s_teeLock
static CRITICAL_SECTION s_teeLock;
static HANDLE s_teeWake = NULL;                       // auto-reset: the pump queued bytes
static volatile LONG s_teeInFlight = 0;               // 1 while the tee thread's WriteFile runs (GXStderrLog_Settle reads it)

static void gxTeeQueue(const char *buf, DWORD len)
{
	EnterCriticalSection(&s_teeLock);
	const size_t room = kTeeRing - (s_teeTail - s_teeHead);
	const size_t take = len < room ? (size_t)len : room;
	for (size_t i = 0; i < take; ++i)
		s_teeRing[(s_teeTail + i) % kTeeRing] = buf[i];
	s_teeTail += take;
	s_teeDropped += (unsigned long long)(len - take);
	LeaveCriticalSection(&s_teeLock);
	SetEvent(s_teeWake);
}

static DWORD WINAPI gxStderrTeeThread(LPVOID)
{
	static char out[65536];
	for (;;)
	{
		WaitForSingleObject(s_teeWake, INFINITE);
		for (;;)
		{
			EnterCriticalSection(&s_teeLock);
			size_t n = s_teeTail - s_teeHead;
			if (n > sizeof(out))
				n = sizeof(out);
			for (size_t i = 0; i < n; ++i)
				out[i] = s_teeRing[(s_teeHead + i) % kTeeRing];
			s_teeHead += n;
			if (n != 0)
				InterlockedExchange(&s_teeInFlight, 1);   // under the lock: Settle never sees "empty and idle" with these bytes unwritten
			LeaveCriticalSection(&s_teeLock);
			if (n == 0)
				break;
			// may block: only this thread waits on the reader. A short write (a nonblocking pipe) continues from where it stopped; a
			// failed or zero write drops the rest and counts it like a full ring (Codex nw1-r2)
			size_t off = 0;
			while (off < n)
			{
				DWORD teed = 0;
				if (!WriteFile(s_teeHandle, out + off, (DWORD)(n - off), &teed, NULL) || teed == 0)
					break;
				off += teed;
			}
			if (off < n)
			{
				EnterCriticalSection(&s_teeLock);
				s_teeDropped += (unsigned long long)(n - off);
				LeaveCriticalSection(&s_teeLock);
			}
			InterlockedExchange(&s_teeInFlight, 0);
		}
	}
}

static unsigned long long gxTeeDroppedNow(void)
{
	EnterCriticalSection(&s_teeLock);
	const unsigned long long d = s_teeDropped;
	LeaveCriticalSection(&s_teeLock);
	return d;
}

/// Drains the pipe that now stands in for stderr.
///
/// Everything read is teed - unfiltered - to whatever stderr pointed at before
/// we intervened, so running the game from a console or with `2> file` behaves
/// exactly as it did. Only the copy that lands in the log file is filtered.
static DWORD WINAPI gxStderrPumpThread(LPVOID)
{
	// Carry a partial trailing line into the next read so GXStderrLog_Consume
	// always sees a chunk that starts on a line boundary - a pipe read can
	// otherwise split "[CRC] MISMATCH..." after three bytes and the classifier
	// would judge the line on a fragment of its own prefix.
	static char buf[16384];
	int carry = 0;
	unsigned long long dropsSaid = 0;

	for (;;)
	{
		DWORD got = 0;
		if (!ReadFile(s_pipeRead, buf + carry, (DWORD)(sizeof(buf) - (size_t)carry), &got, NULL) || got == 0)
			break;

		const int have = carry + (int)got;

		if (s_teeWake != NULL)
			gxTeeQueue(buf + carry, got);

		const char *lastNl = NULL;
		for (int i = have - 1; i >= 0; --i)
		{
			if (buf[i] == '\n') { lastNl = buf + i; break; }
		}

		int consume;
		if (lastNl != NULL)
		{
			consume = (int)(lastNl - buf) + 1;
		}
		else if (have == (int)sizeof(buf))
		{
			// A single line longer than the buffer. Nothing to wait for.
			consume = have;
		}
		else
		{
			carry = have;
			continue;
		}

		GXStderrLog_Consume(buf, consume);

		if (s_teeWake != NULL && buf[consume - 1] == '\n')  // a line of its own: only at a line boundary of the log copy
		{
			const unsigned long long drops = gxTeeDroppedNow();
			if (drops != dropsSaid)
			{
				char note[128];
				const int n = snprintf(note, sizeof(note), "[gx] stderr tee dropped %llu bytes (the original stderr is not draining)\n",
					drops - dropsSaid);
				if (n > 0)
					GXStderrLog_Consume(note, n < (int)sizeof(note) ? n : (int)sizeof(note) - 1);
				dropsSaid = drops;
			}
		}

		carry = have - consume;
		if (carry > 0)
			memmove(buf, buf + consume, (size_t)carry);
	}

	// The pump should outlive everything - the write end stays open for the
	// process lifetime, so the only way here is an unexpected pipe error. If it
	// does happen, stderr must stop pointing at an undrained pipe: 64 KB later
	// the next fprintf would block forever and the diagnostic aid would have
	// become a hang. Send stderr to NUL and give up on the log instead.
	if (s_stderrFd >= 0)
	{
		const int nulFd = _open("NUL", _O_WRONLY);
		if (nulFd >= 0)
		{
			_dup2(nulFd, s_stderrFd);
			_close(nulFd);
		}
	}
	return 0;
}

static bool gxHookStderr(void)
{
	HANDLE hRead = INVALID_HANDLE_VALUE;
	HANDLE hWrite = INVALID_HANDLE_VALUE;
	if (!CreatePipe(&hRead, &hWrite, NULL, 1 << 16))
		return false;

	const int writeFd = _open_osfhandle((intptr_t)hWrite, _O_WRONLY | _O_BINARY);
	if (writeFd < 0)
	{
		CloseHandle(hRead);
		CloseHandle(hWrite);
		return false;
	}

	// A WIN32-subsystem process launched from Explorer has no console, and the
	// CRT reports _fileno(stderr) == -2 for it. _dup2 onto that fails, so give
	// the stream a real descriptor first by pointing it at NUL. When a
	// descriptor DOES already exist - a console, or `2> file` - keep a copy to
	// tee to instead, so nothing that works today stops working.
	int errFd = _fileno(stderr);
	if (errFd >= 0)
	{
		// Keep the OS handle rather than the CRT descriptor: the pump thread is
		// a bare CreateThread and has no business calling into the CRT.
		const int dupFd = _dup(errFd);
		if (dupFd >= 0)
			s_teeHandle = (HANDLE)_get_osfhandle(dupFd);
	}
	else
	{
		if (freopen("NUL", "w", stderr) == NULL)
		{
			_close(writeFd);
			CloseHandle(hRead);
			return false;
		}
		errFd = _fileno(stderr);
		if (errFd < 0)
		{
			_close(writeFd);
			CloseHandle(hRead);
			return false;
		}
	}

	if (_dup2(writeFd, errFd) != 0)
	{
		_close(writeFd);
		CloseHandle(hRead);
		return false;
	}
	_close(writeFd);  // fd 2 now owns a duplicate of the pipe's write handle

	s_stderrFd = errFd;

	// Non-CRT code reaches stderr through GetStdHandle, not through fd 2.
	SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(errFd));

	// MSVC maps _IOLBF onto _IOFBF - there is no line buffering to ask for - so
	// unbuffered is the only setting that satisfies "a crash still leaves the
	// recent lines". The writes land in an in-memory pipe rather than on disk,
	// and stderr is unbuffered by default in this CRT anyway, so this mostly
	// makes an existing property explicit.
	setvbuf(stderr, NULL, _IONBF, 0);

	s_pipeRead = hRead;

	// The tee thread first (D · native-windows-1): without it the pump falls back to no tee at all, never to a blocking one
	if (s_teeHandle != INVALID_HANDLE_VALUE)
	{
		InitializeCriticalSection(&s_teeLock);
		s_teeWake = CreateEventW(NULL, FALSE, FALSE, NULL);
		HANDLE tt = s_teeWake != NULL ? CreateThread(NULL, 0, gxStderrTeeThread, NULL, 0, NULL) : NULL;
		if (tt == NULL)
		{
			if (s_teeWake != NULL)
				CloseHandle(s_teeWake);
			s_teeWake = NULL;
		}
		else
			CloseHandle(tt);
	}

	const HANDLE th = CreateThread(NULL, 0, gxStderrPumpThread, NULL, 0, NULL);
	if (th == NULL)
	{
		// Never leave the redirect in place without a pump: the pipe would fill
		// at 64 KB and the next fprintf(stderr) would block the game forever.
		s_pipeRead = INVALID_HANDLE_VALUE;
		s_stderrFd = -1;
		CloseHandle(hRead);
		freopen("NUL", "w", stderr);
		return false;
	}
	CloseHandle(th);
	return true;
}

void GXStderrLog_Settle(void)
{
	if (!s_installed)
		return;
	// stderr is unbuffered, so the bytes are already in the pipe; what is left
	// is to let the pump thread move them to the file. Deliberately no
	// fflush(stderr) - the faulting thread may not be the one holding the
	// stream lock, and a crash reporter that can deadlock is worse than one
	// that loses a line.
	Sleep(150);
	// D · native-windows-1 13/10/2026 (Codex nw1-r1 P2): the tee writes from its own thread now. Give it the same kind of bounded
	// moment, at most 150 ms more, to empty its ring and finish the write in flight - never blocking: TryEnter only, because the
	// faulting thread may be the one holding the lock. A reader that does not drain loses the tail, as it would have stalled before.
	if (s_teeWake != NULL)
	{
		for (int i = 0; i < 15; ++i)
		{
			bool empty = false;
			if (TryEnterCriticalSection(&s_teeLock))
			{
				empty = s_teeTail == s_teeHead;
				LeaveCriticalSection(&s_teeLock);
			}
			if (empty && InterlockedCompareExchange(&s_teeInFlight, 0, 0) == 0)
				break;
			Sleep(10);
		}
	}
}

int GXStderrLog_GetFd(void)
{
	return -1;
}

#elif defined(__APPLE__)

static void gxDeliver(const char *buf, int len)
{
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	// Mirror, don't move: fd 2 is untouched, so `2> file` and every harness
	// that reads this process's stderr sees exactly what it saw before -
	// unfiltered, because the filter is a property of the log file, not of the
	// program's output. Not done on iOS, where fd 2 goes to the system log and
	// the sink has always been a straight replacement.
	(void)!write(STDERR_FILENO, buf, (size_t)len);
#endif
	GXStderrLog_Consume(buf, len);
}

// GeneralsX @perf Claude 02/10/2026 The write(2) calls above used to run on whichever thread
// printed - in practice the main thread, so a slow disk stalled a frame and, in a match, every peer
// waiting on it (ROADMAP_2027 M2 A1; HITCHES H21). Now the printer copies its bytes into a ring and
// one helper thread, the pump, does both writes, as Windows' pipe pump already does. The order of
// the bytes is unchanged: one ring, one reader.
//
// What must still reach the disk before the process ends, does:
//   * a line starting "FATAL" waits (at most 1 s) until the pump has written it - Debug.cpp's
//     release-crash paths print one and then _exit(1), with no hook of ours in between;
//   * GXStderrLog_Settle() waits for everything queued so far - the crash handlers and the end of
//     main() call it before their own writes and _exit, and exit() runs it from atexit;
//   * a full ring makes the printer wait up to 200 ms for room, then drop and count; until the pump
//     moves again, later printers drop at once instead of each waiting. The pump writes a
//     "[gx] stderr pump dropped" line where it next catches up. 1 MiB at a normal line rate is
//     minutes of a stuck disk or of an undrained stderr pipe, which used to hang the game outright.
// A SIGKILL can lose what is still queued: the last microseconds of output.
// GX_STDERR_PUMP=0 restores the old inline writes.
static const size_t kRingSize = 1u << 20;
static char s_ring[kRingSize];
static std::atomic<uint64_t> s_head(0);  // bytes ever queued; written under s_ringLock
static std::atomic<uint64_t> s_tail(0);  // bytes ever written out; written by the pump only, under s_ringLock
static pthread_mutex_t s_ringLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ringData = PTHREAD_COND_INITIALIZER;  // the pump waits here for bytes
static pthread_cond_t s_ringRoom = PTHREAD_COND_INITIALIZER;  // printers wait here for room or delivery
static bool s_pumpIdle = false;
static bool s_pumpOn = false;
static uint64_t s_dropBytes = 0;  // under s_ringLock
static bool s_stuck = false;       // under s_ringLock: a printer gave up waiting and the pump has not moved since

// GeneralsX @feature Claude 02/10/2026 GX_STDERR_STATS=<s>: every <s> seconds a [GXLOGSTAT] line says what
// printing cost the thread that printed - calls, bytes, and the main thread's total, worst and
// slow-call counts - so a real run shows its line rate and its write tail (ROADMAP_2027 M2 A1).
// The printer-side counters are touched only inside this FILE's write callback, which stdio
// serialises; the pump's are atomics.
static bool s_stats = false;
static uint64_t s_statPeriodNs = 0;
static uint64_t s_statStart = 0;
static uint64_t s_statNext = 0;
static uint64_t s_calls = 0, s_bytes = 0, s_mainCalls = 0, s_mainNs = 0, s_mainMaxNs = 0;
static uint64_t s_mainGe1ms = 0, s_mainGe10ms = 0, s_mainGe50ms = 0, s_otherMaxNs = 0;
static uint64_t s_ringHwm = 0, s_roomWaits = 0, s_urgentWaits = 0;  // under s_ringLock
static std::atomic<uint64_t> s_pumpWrites(0), s_pumpMaxNs(0), s_pumpGe10ms(0);

static uint64_t gxNowNs(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/// Wait on cond until the absolute CLOCK_UPTIME_RAW deadline. Darwin has no pthread_condattr_setclock,
/// so the relative wait is the monotonic one. False once the deadline has passed.
static bool gxWaitUntil(pthread_cond_t *cond, uint64_t deadlineNs)
{
	const uint64_t now = gxNowNs();
	if (now >= deadlineNs)
		return false;
	const uint64_t left = deadlineNs - now;
	struct timespec rel;
	rel.tv_sec = (time_t)(left / 1000000000u);
	rel.tv_nsec = (long)(left % 1000000000u);
	pthread_cond_timedwait_relative_np(cond, &s_ringLock, &rel);
	return true;
}

static void gxQueue(const char *buf, int len, bool waitWritten)
{
	pthread_mutex_lock(&s_ringLock);
	const uint64_t roomDeadline = gxNowNs() + 200000000u;
	size_t off = 0;
	while (off < (size_t)len)
	{
		const uint64_t head = s_head.load(std::memory_order_relaxed);
		const size_t room = kRingSize - (size_t)(head - s_tail.load(std::memory_order_relaxed));
		// Whole writes or nothing: a part-queued write would leave a half line for the next one to be glued onto,
		// and the classifier would judge that line by the wrong prefix. Only a write bigger than the ring goes in
		// ring-sized pieces.
		const size_t need = ((size_t)len - off < kRingSize) ? (size_t)len - off : kRingSize;
		if (room < need)
		{
			++s_roomWaits;
			if (s_stuck || !gxWaitUntil(&s_ringRoom, roomDeadline))
			{
				s_stuck = true;
				s_dropBytes += (uint64_t)len - off;
				break;
			}
			continue;
		}
		const size_t n = ((size_t)len - off < room) ? (size_t)len - off : room;
		const size_t at = (size_t)(head % kRingSize);
		const size_t first = (n < kRingSize - at) ? n : kRingSize - at;
		memcpy(s_ring + at, buf + off, first);
		memcpy(s_ring, buf + off + first, n - first);
		s_head.store(head + n, std::memory_order_release);
		off += n;
		const uint64_t used = head + n - s_tail.load(std::memory_order_relaxed);
		if (used > s_ringHwm) s_ringHwm = used;
		if (s_pumpIdle)
			pthread_cond_signal(&s_ringData);
	}
	if (waitWritten && !s_stuck)
	{
		++s_urgentWaits;
		const uint64_t want = s_head.load(std::memory_order_relaxed);
		const uint64_t deadline = gxNowNs() + 1000000000u;
		while (s_tail.load(std::memory_order_relaxed) < want && gxWaitUntil(&s_ringRoom, deadline))
		{
		}
	}
	pthread_mutex_unlock(&s_ringLock);
}

static void *gxPumpThread(void *)
{
	pthread_setname_np("gx-stderr-pump");
	// Bytes are copied out so the ring's wrap never splits what GXStderrLog_Consume sees; a cut that
	// is forced by this buffer's size lands after a newline, so a line's prefix stays whole.
	static char chunk[64 * 1024];
	uint64_t dropsSaid = 0;
	char lastOut = '\n';  // the last byte written out: the drop marker starts a line of its own
	pthread_mutex_lock(&s_ringLock);
	for (;;)
	{
		const uint64_t tail = s_tail.load(std::memory_order_relaxed);
		const uint64_t head = s_head.load(std::memory_order_relaxed);
		const uint64_t drops = s_dropBytes;
		if (head == tail && drops == dropsSaid)
		{
			s_pumpIdle = true;
			pthread_cond_wait(&s_ringData, &s_ringLock);
			s_pumpIdle = false;
			continue;
		}
		size_t n = (head - tail < sizeof(chunk)) ? (size_t)(head - tail) : sizeof(chunk);
		const size_t at = (size_t)(tail % kRingSize);
		const size_t first = (n < kRingSize - at) ? n : kRingSize - at;
		memcpy(chunk, s_ring + at, first);
		memcpy(chunk + first, s_ring, n - first);
		pthread_mutex_unlock(&s_ringLock);

		if (head - tail > n)
		{
			const char *nl = NULL;
			for (size_t i = n; i > 0; --i)
			{
				if (chunk[i - 1] == '\n') { nl = chunk + i - 1; break; }
			}
			if (nl != NULL)
				n = (size_t)(nl - chunk) + 1;
		}
		const uint64_t t0 = gxNowNs();
		if (n > 0)
		{
			gxDeliver(chunk, (int)n);
			lastOut = chunk[n - 1];
		}
		if (drops != dropsSaid)
		{
			char mark[128];
			const int m = snprintf(mark, sizeof(mark),
				"%s[gx] stderr pump dropped %" PRIu64 " bytes (stderr not draining)\n", lastOut == '\n' ? "" : "\n",
				drops - dropsSaid);
			lastOut = '\n';
			if (m > 0)
				gxDeliver(mark, m < (int)sizeof(mark) ? m : (int)sizeof(mark) - 1);
			dropsSaid = drops;
		}
		const uint64_t ns = gxNowNs() - t0;
		s_pumpWrites.fetch_add(1, std::memory_order_relaxed);
		if (ns > s_pumpMaxNs.load(std::memory_order_relaxed)) s_pumpMaxNs.store(ns, std::memory_order_relaxed);
		if (ns >= 10000000u) s_pumpGe10ms.fetch_add(1, std::memory_order_relaxed);

		pthread_mutex_lock(&s_ringLock);
		s_tail.store(tail + n, std::memory_order_release);
		s_stuck = false;
		pthread_cond_broadcast(&s_ringRoom);
	}
	return NULL;
}

static void gxEmit(const char *buf, int len, bool waitWritten)
{
	if (s_pumpOn)
		gxQueue(buf, len, waitWritten);
	else
		gxDeliver(buf, len);
}

static void gxStatAccount(uint64_t t0, int len)
{
	const uint64_t now = gxNowNs();
	const uint64_t ns = now - t0;
	++s_calls;
	s_bytes += (uint64_t)len;
	if (pthread_main_np())
	{
		++s_mainCalls;
		s_mainNs += ns;
		if (ns > s_mainMaxNs) s_mainMaxNs = ns;
		s_mainGe1ms += ns >= 1000000u;
		s_mainGe10ms += ns >= 10000000u;
		s_mainGe50ms += ns >= 50000000u;
	}
	else if (ns > s_otherMaxNs)
	{
		s_otherMaxNs = ns;
	}
	if (now < s_statNext)
		return;
	s_statNext = now + s_statPeriodNs;
	pthread_mutex_lock(&s_ringLock);
	const uint64_t hwm = s_ringHwm, roomWaits = s_roomWaits, urgentWaits = s_urgentWaits, drops = s_dropBytes;
	pthread_mutex_unlock(&s_ringLock);
	char line[480];
	const int n = snprintf(line, sizeof(line),
		"[GXLOGSTAT] up_s=%" PRIu64 " calls=%" PRIu64 " bytes=%" PRIu64 " main_calls=%" PRIu64
		" main_us=%" PRIu64 " main_max_us=%" PRIu64 " main_ge1ms=%" PRIu64 " main_ge10ms=%" PRIu64
		" main_ge50ms=%" PRIu64 " other_max_us=%" PRIu64 " mode=%s pump_writes=%" PRIu64
		" pump_max_us=%" PRIu64 " pump_ge10ms=%" PRIu64 " ring_hwm=%" PRIu64 " room_waits=%" PRIu64
		" urgent_waits=%" PRIu64 " dropped=%" PRIu64 "\n",
		(now - s_statStart) / 1000000000u, s_calls, s_bytes, s_mainCalls, s_mainNs / 1000u,
		s_mainMaxNs / 1000u, s_mainGe1ms, s_mainGe10ms, s_mainGe50ms, s_otherMaxNs / 1000u,
		s_pumpOn ? "pump" : "sync", s_pumpWrites.load(std::memory_order_relaxed),
		s_pumpMaxNs.load(std::memory_order_relaxed) / 1000u, s_pumpGe10ms.load(std::memory_order_relaxed),
		hwm, roomWaits, urgentWaits, drops);
	if (n > 0)
		gxEmit(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1, false);
}

// The line the printers are in the middle of, across callbacks: stdio hands a line longer than its buffer over in
// several pieces, and every piece of a FATAL line must wait, not only the first (review of a1a4ba276, P2: a 3000-byte
// release-crash message kept 995 bytes in the log before _exit). Touched only in the callback, which stdio serialises.
static bool s_sinkAtLineStart = true;
static bool s_sinkFatalLine = false;

/// True if buf holds any byte of a line that starts with "FATAL", including the later pieces of one.
static bool gxTouchesFatalLine(const char *buf, int len)
{
	static const char kFatal[] = "FATAL";
	bool touched = false;
	int i = 0;
	while (i < len)
	{
		if (s_sinkAtLineStart)
		{
			// A piece that ends inside the first five bytes of a line counts if it could still become "FATAL".
			const int have = len - i;
			const int n = have < 5 ? have : 5;
			s_sinkFatalLine = memcmp(buf + i, kFatal, (size_t)n) == 0;
			s_sinkAtLineStart = false;
		}
		touched = touched || s_sinkFatalLine;
		const char *nl = (const char *)memchr(buf + i, '\n', (size_t)(len - i));
		if (nl == NULL)
			break;
		i = (int)(nl - buf) + 1;
		s_sinkAtLineStart = true;
	}
	return touched;
}

static int gxSinkWrite(void *, const char *buf, int len)
{
	const uint64_t t0 = s_stats ? gxNowNs() : 0;
	gxEmit(buf, len, s_pumpOn && gxTouchesFatalLine(buf, len));
	if (s_stats)
		gxStatAccount(t0, len);
	return len;  // always report the write consumed; a dropped line is not an error
}

static void gxSettleAtExit(void)
{
	GXStderrLog_Settle();
}

static bool gxHookStderr(void)
{
	const char *stats = getenv("GX_STDERR_STATS");
	const int period = stats != NULL ? atoi(stats) : 0;
	s_stats = period > 0;
	s_statPeriodNs = (uint64_t)period * 1000000000ull;
	s_statStart = gxNowNs();
	s_statNext = s_statStart + s_statPeriodNs;

	FILE *sink = funopen(NULL, NULL, gxSinkWrite, NULL, NULL);
	if (sink == NULL)
		return false;

	// The pump before the swap: once stderr is swapped, every line goes where s_pumpOn says.
	// A bare pthread, not std::thread: nothing here may allocate through the game's operator new.
	const char *pump = getenv("GX_STDERR_PUMP");
	if (!(pump != NULL && pump[0] == '0' && pump[1] == '\0'))
	{
		pthread_t th;
		if (pthread_create(&th, NULL, gxPumpThread, NULL) == 0)
		{
			pthread_detach(th);
			s_pumpOn = true;
			atexit(gxSettleAtExit);
		}
	}

	*stderr = *sink;  // classic Darwin stderr swap; stderr is a FILE, not a macro
	setvbuf(stderr, NULL, _IOLBF, 0);  // line-buffered so a crash still flushes recent lines
	return true;
}

void GXStderrLog_Settle(void)
{
	// Async-signal-safe: atomics and nanosleep only, never s_ringLock - the crashing thread may hold it.
	if (!s_pumpOn)
		return;
	const uint64_t want = s_head.load(std::memory_order_acquire);
	for (int i = 0; i < 1000 && s_tail.load(std::memory_order_acquire) < want; ++i)
	{
		struct timespec ms = { 0, 1000000 };
		nanosleep(&ms, NULL);
	}
}

int GXStderrLog_GetFd(void)
{
	return s_logFd;
}

#else  // Linux and anything else

static bool gxHookStderr(void)
{
	return false;
}

void GXStderrLog_Settle(void) {}

int GXStderrLog_GetFd(void)
{
	return s_logFd;
}

#endif

bool GXStderrLog_Install(const char *dir)
{
	if (!GX_STDERR_SINK_SUPPORTED)
		return false;  // checked before the file is created, so none is left behind
	if (s_installed)
		return false;  // iOS installs its own into Documents before this runs
	if (dir == NULL || dir[0] == '\0')
		return false;

	const char *opt = getenv("GX_STDERR_LOG");
	if (opt != NULL && opt[0] == '0' && opt[1] == '\0')
		return false;

	if (!gxOpenLogFile(dir))
		return false;

	if (!gxHookStderr())
	{
#ifdef _WIN32
		if (s_logHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(s_logHandle);
			s_logHandle = INVALID_HANDLE_VALUE;
		}
#else
		if (s_logFd >= 0)
		{
			close(s_logFd);
			s_logFd = -1;
		}
#endif
		return false;
	}

	s_installed = true;
	return true;
}
