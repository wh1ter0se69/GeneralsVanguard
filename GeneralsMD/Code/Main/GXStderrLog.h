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

// Make a desync leave evidence on disk.
//
// A release build prints "[CRC] MISMATCH" and each player's CRC to stderr on a desync, but a Windows GUI
// process has no console and a Finder-launched Mac has stderr on /dev/null.
// This installs a filtered, size-capped MIRROR of stderr next to the release crash log; stderr is unchanged.

#pragma once

/**
 * Install the stderr mirror, writing generals-stderr.log (a previous one rotates to generals-stderr-prev.log).
 * GX_STDERR_LOG=0 opts out. Idempotent: later calls are no-ops returning false (iOS installs its own sink earlier).
 *
 * @param dir  Directory to write into, INCLUDING its trailing separator; null or empty installs nothing.
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
 * Windows: a helper thread drains the pipe, so call this at the top of the unhandled-exception filter.
 * Darwin: waits up to 1 s for queued lines; async-signal-safe, so call it from a fatal-signal handler.
 * No-op on Linux, and on Darwin under GX_STDERR_PUMP=0.
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
