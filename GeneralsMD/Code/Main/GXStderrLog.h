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

// FILE: GXStderrLog.h ////////////////////////////////////////////////////////
//
// GeneralsX: Make a desync leave evidence on disk.
//
// GameLogic.cpp:3000 already prints, in RELEASE builds, the frame and every
// player's CRC the moment a desync is detected:
//
//     [CRC] MISMATCH frame=%u crcs=%u players=%d
//     [CRC]   player=%d crc=%08X
//
// That names the diverging peer. It has never been read, because it goes to
// stderr and the two platforms real players use have nowhere for stderr to go:
// a Windows GUI-subsystem process has no console, and a Mac launched from
// Finder has stderr wired to /dev/null. A real 3-player desync on 2026-08-16
// left exactly ONE boolean byte in the .rep header as evidence, and eight
// candidate causes could be excluded but none confirmed.
//
// This installs a filtered, size-capped MIRROR of stderr into a file next to
// the release crash log. It is a mirror, not a redirect: everything still
// reaches the original stderr, so the Linux harnesses and any `2>` redirect
// keep working byte for byte.
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

/**
 * Install the stderr mirror.
 *
 * @param dir  Directory to write into, INCLUDING its trailing separator - pass
 *             TheGlobalData->getPath_UserData().str(), which is what the release
 *             crash log (Debug.cpp, RELEASECRASH_FILE_NAME) uses. May be null or
 *             empty, in which case nothing is installed.
 *
 * Writes "generals-stderr.log", rotating any previous one to
 * "generals-stderr-prev.log" first.
 *
 * Idempotent: the second and later calls are no-ops and return false. That is
 * load-bearing on iOS, which installs its own sink into Documents long before
 * TheGlobalData exists and must not be re-pointed at the container's Library.
 *
 * Honours GX_STDERR_LOG=0 as an opt-out.
 *
 * @return true if this call installed the sink.
 */
bool GXStderrLog_Install(const char *dir);

/**
 * Raw descriptor of the log file, for async-signal-safe crash writers that
 * cannot go through a FILE*. -1 when no sink is installed.
 *
 * Windows has no POSIX descriptor here and always returns -1; its crash path is
 * the minidump, not a signal handler.
 */
int GXStderrLog_GetFd(void);

/**
 * Give the sink a moment to reach disk before the process is torn down.
 *
 * On Windows the mirror is fed through a pipe drained by a helper thread, so a
 * few bytes can still be in flight when a crash handler runs. Call this at the
 * top of the unhandled-exception filter. Deliberately does NOT touch stderr's
 * FILE lock: the faulting thread may not be the one holding it.
 *
 * Darwin (GeneralsX:): the sink queues lines for a pump
 * thread, so this waits - at most 1 s - until everything queued before the call
 * is written. Async-signal-safe (atomics and nanosleep): call it at the top of a
 * fatal-signal handler and before _exit(). exit() already runs it.
 *
 * No-op on Linux, and on Darwin under GX_STDERR_PUMP=0 (inline writes).
 */
void GXStderrLog_Settle(void);

/**
 * Feed bytes to the sink's filter/cap policy. Public only so the policy can be
 * exercised by a test; the platform plumbing calls it for you.
 *
 * Chunks may contain any number of whole or partial lines; the classifier keeps
 * its own line state across calls.
 */
void GXStderrLog_Consume(const char *buf, int len);
