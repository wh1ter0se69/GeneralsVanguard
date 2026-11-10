// GeneralsX: open a file for writing that only its owner can read.
//
// The sign-in assertion (GXLoginFlow.cpp) and GXPlayer.ini (GXPlayerIdentity.cpp, a bearer credential: whoever holds the id
// is the player) were written with fopen(path, "w"), so they came out 0644 under the usual umask - readable by every other
// local user of a shared macOS or Linux machine whose home is traversable. This opens the same path with mode 0600 instead,
// and fchmods it too, because O_TRUNC keeps the mode of a .tmp an older build left behind. rename() keeps the mode, so the
// file put in place is 0600 as well. Windows profile folders are already per-user: plain fopen there.
//
// The name is unchanged on purpose: release.rs's writers contract pins "<final>.tmp" for these writers (GXReplaceFile.h).
// scripts/test/private-file-test.cpp drives it with `c++ -std=c++17 -I Core/GameEngine/Include`. Not in the simulation
// digest: nothing digested includes it.
#pragma once

#include <cstdio>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

inline FILE *gxOpenPrivate(const char *path, const char *mode)
{
#ifdef _WIN32
	return fopen(path, mode);
#else
	const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return nullptr;
	if (fchmod(fd, 0600) != 0)
	{
		close(fd);
		return nullptr;
	}
	FILE *fp = fdopen(fd, mode);
	if (fp == nullptr)
		close(fd);
	return fp;
#endif
}
