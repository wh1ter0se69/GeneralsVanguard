// GeneralsX: removing a saved sign-in from disk, as a PURE function.
//
// PURE ON PURPOSE, the same way GXOnlineGate.h is: no engine, no AsciiString, no TheGlobalData, so
// scripts/test/signout-files-test.cpp can drive it against real files in a scratch directory. The
// properties it exists to hold are the ones a sign-out is judged by, and none of them is visible
// from inside the running game:
//
//   1. NO COPY IS LEFT UNREPORTED. GXLoginFlow writes "<file>.tmp" and renames it into place, so a
//      crash between the two leaves a complete, valid assertion in the .tmp. Nothing reads it back,
//      so it signs nobody in - but it is a bearer token on disk, and a sign-out that leaves it has
//      not removed the sign-in from the computer. Both files go, and if EITHER survives the answer
//      says so. (A first version returned "gone" whenever the main file went, whatever happened to
//      the .tmp - found by cross-model review, 18/09/2026.)
//
//   2. THE ANSWER IS TRUE. The two survivals are different facts and are kept apart, because the
//      sentence each one produces is different: a surviving GXLogin.txt signs the player back in at
//      the next launch, a surviving .tmp never does. A file that was ALREADY absent - deleted by
//      hand, or a second sign-out - is GONE, not a failure: remove() alone reports it as a failure,
//      which is what the code did before this existed.
//
// Not in the simulation digest: Core/GameEngine/Include/GameClient is not a digested root.
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
