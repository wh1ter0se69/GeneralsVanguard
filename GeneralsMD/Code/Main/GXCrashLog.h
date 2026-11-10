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

// FILE: GXCrashLog.h /////////////////////////////////////////////////////////
//
// GeneralsX: One readable crash log per crash, in OUR folder.
//
// Before this, per platform (read on 13c750034):
//   Windows  a minidump in <userdata>\CrashDumps\ (MiniDumper.cpp) - ours, but not readable.
//   iOS      a backtrace appended to Documents/generals-stderr.log, which the next launch rotates.
//   Linux    the handlers in SDL3Main.cpp were compiled but never INSTALLED (the only call was
//            inside the iOS block), so a crash left the kernel's one dmesg line.
//   macOS    no handler at all: the only record was the OS's .ips report under
//            ~/Library/Logs/DiagnosticReports - another program's folder, full of home paths.
//
// Now every platform writes <userdata>/CrashLogs/crash-<unix time>-<pid>.txt: a short header
// (format tag, build, reason, time) and the backtrace, with every directory stripped from every
// path so no home folder or user name is written. The launcher reads this folder and asks the
// player before anything leaves the machine (gxio::crashlogs).
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stddef.h>

/**
 * Remember where crash logs go. Creates <dir>CrashLogs/.
 *
 * @param dir    user-data root INCLUDING its trailing separator (getPath_UserData()).
 * @param build  one line naming the build, e.g. "GeneralsXZH 13c750034 macos arm64". Copied.
 *
 * First successful call wins; later calls return false (iOS installs early, into Documents).
 * Not async-signal-safe: call at startup.
 */
bool GXCrashLog_Install(const char *dir, const char *build);

/**
 * Write one crash log. POSIX: async-signal-safe (open/write/close, backtrace_symbols_fd into a
 * pipe opened at install). Windows: resolves each frame to module+offset.
 *
 * @param reason  one line, e.g. "SIGSEGV (invalid memory access)".
 * @param frames  return addresses, may be null when n == 0.
 * @return true if a file was written. False when not installed.
 */
bool GXCrashLog_Write(const char *reason, void *const *frames, int n);

/**
 * Strip every directory from every path in s[0..n), in place: a token (split on whitespace and
 * ()[]<>"',;=) that contains '/' or '\\' keeps only what follows the last one.
 * "/home/me/GX/GeneralsXZH(+0x1)" -> "GeneralsXZH(+0x1)". Returns the new length.
 * Pure and async-signal-safe; public so scripts/test/crash-log-test.sh can test it.
 */
size_t GXCrashLog_Scrub(char *s, size_t n);
