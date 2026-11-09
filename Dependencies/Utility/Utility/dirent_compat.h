/*
 * GeneralsX @bugfix Claude 27/07/2026 <dirent.h> shim for Windows.
 *
 * GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp walks the save directory with the POSIX
 * opendir/readdir/closedir trio (the -autoload probe). Those do not exist in the MSVC CRT, so the
 * unguarded "#include <dirent.h>" was a hard C1083 on Windows.
 *
 * Rather than fork the walk, provide the three calls on top of _findfirst/_findnext/_findclose,
 * which the MSVC CRT has always shipped in <io.h>. Only the members that walk actually touches are
 * provided - d_name - because a fuller emulation would be dead code we could not test.
 *
 * This header is Windows-only by construction: everything below is inside #ifdef _WIN32, so
 * including it on any other platform yields an empty translation unit. Call sites still include
 * the real <dirent.h> off Windows.
 */
#pragma once

#ifdef _WIN32

#include <io.h>
#include <stdlib.h>
#include <string.h>

#ifndef NAME_MAX
#define NAME_MAX 260
#endif

struct dirent
{
	char d_name[NAME_MAX + 1];
};

typedef struct GX_DIR
{
	intptr_t       m_handle;   /* -1 once exhausted or never opened */
	struct _finddata_t m_find;
	int            m_pending;  /* the entry in m_find has not been returned yet */
	struct dirent  m_entry;
} DIR;

/* Returns NULL when the directory cannot be opened, matching opendir(). */
inline DIR *opendir(const char *path)
{
	if (path == NULL)
	{
		return NULL;
	}

	/* _findfirst wants a wildcard, and the caller hands us a bare directory. Tolerate a trailing
	   separator so both "C:\saves" and "C:\saves\" work - getFilePathInSaveDirectory("") produces
	   the latter. */
	size_t len = strlen(path);
	char *pattern = (char *)malloc(len + 3);
	if (pattern == NULL)
	{
		return NULL;
	}
	memcpy(pattern, path, len);
	if (len == 0 || (path[len - 1] != '\\' && path[len - 1] != '/'))
	{
		pattern[len++] = '\\';
	}
	pattern[len++] = '*';
	pattern[len] = '\0';

	DIR *dir = (DIR *)calloc(1, sizeof(DIR));
	if (dir == NULL)
	{
		free(pattern);
		return NULL;
	}

	dir->m_handle = _findfirst(pattern, &dir->m_find);
	free(pattern);

	if (dir->m_handle == -1)
	{
		free(dir);
		return NULL;
	}

	dir->m_pending = 1;
	return dir;
}

/* Returns NULL at end of directory, matching readdir(). The returned pointer is owned by the DIR
   and is invalidated by the next readdir()/closedir(), exactly like the POSIX contract. */
inline struct dirent *readdir(DIR *dir)
{
	if (dir == NULL || dir->m_handle == -1)
	{
		return NULL;
	}

	if (!dir->m_pending)
	{
		if (_findnext(dir->m_handle, &dir->m_find) != 0)
		{
			return NULL;
		}
	}
	dir->m_pending = 0;

	strncpy(dir->m_entry.d_name, dir->m_find.name, NAME_MAX);
	dir->m_entry.d_name[NAME_MAX] = '\0';
	return &dir->m_entry;
}

inline int closedir(DIR *dir)
{
	if (dir == NULL)
	{
		return -1;
	}

	int result = 0;
	if (dir->m_handle != -1)
	{
		result = _findclose(dir->m_handle);
	}
	free(dir);
	return result;
}

/* <strings.h> travels with <dirent.h> at the same call sites, for strcasecmp. */
#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#endif /* _WIN32 */
