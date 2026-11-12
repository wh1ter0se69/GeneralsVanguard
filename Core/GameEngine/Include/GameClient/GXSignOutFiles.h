/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// Removing a saved sign-in from disk, as a PURE function (scripts/test/signout-files-test.cpp drives it on real files).
// NO COPY IS LEFT UNREPORTED: a crash between GXLoginFlow's write and rename leaves a bearer token in "<file>.tmp".
// Both files go; if EITHER survives the answer says which: GXLogin.txt signs back in, a .tmp never does.
// A file ALREADY absent is GONE, not a failure. Not in the simulation digest.
#pragma once

#include <cerrno>
#include <cstdio>
#include <string>

enum GXSignInRemoval
{
	GX_SIGNIN_REMOVED = 0,		///< neither file remains - including when there was none to begin with
	GX_SIGNIN_FILE_SURVIVED,	///< GXLogin.txt is still there: the player is signed in again next launch
	GX_SIGNIN_COPY_SURVIVED		///< only the .tmp is still there: a copy on disk, never read back
};

/// Is `path` gone after this call? remove() succeeding and "it was never there" both count.
inline bool gxRemoveGone( const char *path )
{
	return (std::remove(path) == 0) || (errno == ENOENT);
}

/// Delete the assertion file at `path` and its interrupted-write sibling "<path>.tmp".
///
/// Both are always attempted, whatever happens to the first. The main file's survival is reported
/// in preference to the copy's, because it is the one with a consequence the player will meet.
inline GXSignInRemoval gxRemoveSavedSignIn( const char *path )
{
	if (path == nullptr || path[0] == '\0')
		return GX_SIGNIN_REMOVED;	// no path, no file - nothing can be left behind

	const bool mainGone = gxRemoveGone(path);
	const std::string tmp = std::string(path) + ".tmp";
	const bool copyGone = gxRemoveGone(tmp.c_str());

	if (!mainGone)
		return GX_SIGNIN_FILE_SURVIVED;
	if (!copyGone)
		return GX_SIGNIN_COPY_SURVIVED;
	return GX_SIGNIN_REMOVED;
}
