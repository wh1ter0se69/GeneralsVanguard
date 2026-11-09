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
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// GeneralsX @feature Claude 27/07/2026
// 32-bit digest of the simulation-relevant source tree plus the build
// configuration, computed at build time by simsourcedigest_watcher.cmake.
// 0 is a RESERVED SENTINEL meaning "no provenance" - the build had no git
// information available (source tarball, or configured with
// -DSIMDIGEST_ALLOW_NO_GIT=ON). It must never be compared for equality.
extern const unsigned int SimSourceDigestZeroHour;
extern const unsigned int SimSourceDigestGenerals;

#ifdef __cplusplus
}
#endif
