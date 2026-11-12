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

// FILE: GXCrashLog.cpp ///////////////////////////////////////////////////////
// See GXCrashLog.h. No engine headers on purpose: the
// writer runs inside a signal handler, and scripts/test/crash-log-test.sh compiles this file alone.
///////////////////////////////////////////////////////////////////////////////

#include "GXCrashLog.h"

#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
static const char kSep = '\\';
#else
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
static const char kSep = '/';
static int s_pipe[2] = { -1, -1 };  // backtrace_symbols_fd -> scrub -> file
#endif

static char s_dir[1024];    // "<userdata>CrashLogs/"
static char s_head[512];    // "build: ...\nimage: ...\n", fixed at install
static bool s_installed = false;

// ---------------------------------------------------------------------------
// Scrub. The one privacy property of the file: no directory ever reaches it.
// ---------------------------------------------------------------------------

static bool isDelim(char c)
{
	switch (c) {
		case ' ': case '\t': case '\n': case '\r': case '(': case ')': case '[': case ']':
		case '<': case '>': case '"': case '\'': case ',': case ';': case '=':
			return true;
		default:
			return false;
	}
}

size_t GXCrashLog_Scrub(char *s, size_t n)
{
	size_t out = 0, tok = 0;  // tok: where the current token starts in the output
	for (size_t i = 0; i < n; ++i) {
		const char c = s[i];
		if (c == '/' || c == '\\') { out = tok; continue; }  // drop the token so far, separator included
		s[out++] = c;
		if (isDelim(c)) tok = out;
	}
	return out;
}

// ---------------------------------------------------------------------------
// A fixed buffer and three appenders. No printf: the POSIX caller is a signal handler.
// ---------------------------------------------------------------------------

struct Buf { char b[2048]; size_t n; };

static void put(Buf &b, const char *s)
{
	while (s != nullptr && *s != '\0' && b.n < sizeof(b.b)) b.b[b.n++] = *s++;
}

static void putNum(Buf &b, unsigned long long v, unsigned base, int width)
{
	char t[32];
	int i = 0;
	do { t[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v != 0 && i < 32);
	while (i < width && i < 32) t[i++] = '0';
	while (i > 0) { char c = t[--i]; if (b.n < sizeof(b.b)) b.b[b.n++] = c; }
}

static const char *baseName(const char *p)
{
	const char *r = p;
	for (; p != nullptr && *p != '\0'; ++p) if (*p == '/' || *p == '\\') r = p + 1;
	return r;
}

// ---------------------------------------------------------------------------

bool GXCrashLog_Install(const char *dir, const char *build)
{
	if (s_installed || dir == nullptr || dir[0] == '\0') return false;
	Buf d = {};
	put(d, dir);
	put(d, "CrashLogs");
	if (d.n + 2 >= sizeof(s_dir)) return false;
	memcpy(s_dir, d.b, d.n);
	s_dir[d.n] = '\0';
#ifdef _WIN32
	if (!CreateDirectoryA(s_dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#else
	// Private: a log is written before the launcher redacts it. The
	// chmod covers a folder an earlier build made 0755.
	if (mkdir(s_dir, 0700) != 0 && errno != EEXIST) return false;
	(void)chmod(s_dir, 0700);
	if (pipe(s_pipe) != 0) return false;
	fcntl(s_pipe[0], F_SETFL, fcntl(s_pipe[0], F_GETFL) | O_NONBLOCK);
	fcntl(s_pipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(s_pipe[1], F_SETFD, FD_CLOEXEC);
	// Warm the unwinder here, where allocating is allowed (glibc dlopens libgcc on first use).
	void *warm[4];
	(void)backtrace(warm, 4);
#endif
	s_dir[d.n] = kSep;
	s_dir[d.n + 1] = '\0';

	Buf h = {};
	put(h, "build: ");
	put(h, build);
	put(h, "\n");
#ifndef _WIN32
	// Where the executable was loaded, so an address in the backtrace can be symbolised without
	// the OS's own report. Basename only, like everything else in the file.
	Dl_info info;
	if (dladdr((void *)&GXCrashLog_Install, &info) != 0 && info.dli_fname != nullptr) {
		put(h, "image: ");
		put(h, baseName(info.dli_fname));
		put(h, " base 0x");
		putNum(h, (unsigned long long)(uintptr_t)info.dli_fbase, 16, 1);
		put(h, "\n");
	}
#endif
	const size_t hn = h.n < sizeof(s_head) - 1 ? h.n : sizeof(s_head) - 1;
	memcpy(s_head, h.b, hn);
	s_head[hn] = '\0';
	s_installed = true;
	return true;
}

// crash-<10-digit unix time>-<pid>.txt: sorts by time, and is the only shape the launcher lists.
static bool crashLogPath(Buf &p, unsigned long long stamp)
{
	p.n = 0;
	put(p, s_dir);
	put(p, "crash-");
	putNum(p, stamp, 10, 10);
	put(p, "-");
#ifdef _WIN32
	putNum(p, GetCurrentProcessId(), 10, 1);
#else
	putNum(p, (unsigned long long)getpid(), 10, 1);
#endif
	put(p, ".txt");
	if (p.n >= sizeof(p.b)) return false;
	p.b[p.n] = '\0';
	return true;
}

// A process can write more than one: a stopped save load writes one
// and plays on (SaveLoadFeedback.cpp), and a real crash in the same second would then find its name
// taken. A taken name moves the stamp on a second, a few times, rather than lose the log; the header's
// time: line stays the true one.
static const int kNameTries = 8;

bool GXCrashLog_Write(const char *reason, void *const *frames, int n)
{
	if (!s_installed) return false;
	const unsigned long long now = (unsigned long long)time(nullptr);
	Buf p = {};

	Buf h = {};
	put(h, "GXCRASH1\n");
	put(h, s_head);
	put(h, "reason: ");
	put(h, reason);
	put(h, "\ntime: ");
	putNum(h, now, 10, 1);
	put(h, "\nbacktrace:\n");

#ifdef _WIN32
	HANDLE f = INVALID_HANDLE_VALUE;
	for (int k = 0; k < kNameTries && f == INVALID_HANDLE_VALUE; ++k) {
		if (!crashLogPath(p, now + (unsigned long long)k)) return false;
		f = CreateFileA(p.b, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return false;
	}
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	WriteFile(f, h.b, (DWORD)h.n, &w, nullptr);
	for (int i = 0; i < n; ++i) {
		// module basename + offset: what a PDB symbolises, and no path.
		Buf l = {};
		HMODULE m = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)frames[i], &m) && m != nullptr) {
			char name[MAX_PATH];
			const DWORD len = GetModuleFileNameA(m, name, MAX_PATH);
			name[len < MAX_PATH ? len : MAX_PATH - 1] = '\0';
			put(l, baseName(name));
			put(l, "+0x");
			putNum(l, (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m), 16, 1);
		} else {
			put(l, "0x");
			putNum(l, (unsigned long long)(uintptr_t)frames[i], 16, 1);
		}
		put(l, "\n");
		WriteFile(f, l.b, (DWORD)l.n, &w, nullptr);
	}
	WriteFile(f, "end\n", 4, &w, nullptr);
	CloseHandle(f);
	return true;
#else
	int fd = -1;
	for (int k = 0; k < kNameTries && fd < 0; ++k) {
		if (!crashLogPath(p, now + (unsigned long long)k)) return false;
		fd = open(p.b, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0 && errno != EEXIST) return false;
	}
	if (fd < 0) return false;
	(void)!write(fd, h.b, h.n);
	for (int i = 0; i < n; ++i) {
		// One frame at a time, so one read always holds the whole line and a path is never split
		// across two scrub calls. Trade-off: a single symbol line over 4 KiB would be split and its
		// head written unscrubbed; mangled template names have not come close.
		backtrace_symbols_fd(&frames[i], 1, s_pipe[1]);
		char line[4096];
		ssize_t r;
		while ((r = read(s_pipe[0], line, sizeof(line))) > 0) {
			(void)!write(fd, line, GXCrashLog_Scrub(line, (size_t)r));
		}
	}
	(void)!write(fd, "end\n", 4);
	close(fd);
	return true;
#endif
}
