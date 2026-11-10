#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <string.h>  // GeneralsX @build fbraz 10/02/2026 - strlen, memcpy for strlcpy/strlcat

typedef const char* LPCSTR;
typedef char* LPSTR;

extern "C"
{
char* itoa(int value, char* str, int base);

int _vsnwprintf(wchar_t* buffer, size_t count, const wchar_t* format, va_list args);
// GeneralsX @build fbraz 11/02/2026 - Removed _strlwr declaration (conflicts with Utility inline definition)
// GeneralsX @build fbraz 11/02/2026 - _strupr implemented in string_compat.cpp as weak symbol
char* _strupr(char* str);
}
#define _vsnprintf vsnprintf
#define _snprintf snprintf
#define strlwr _strlwr
#define strupr _strupr

#define lstrcat strcat
#define lstrcpy strcpy
#define lstrcpyn strncpy
#define lstrcmpi strcasecmp
#define lstrlen strlen
#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp
#define _strdup strdup

// GeneralsX @build fbraz 10/02/2026 Bender
// NOTE: strlcpy/strlcat are not defined here: WWLib/stringex.h already defines them, and
// WWCommon.h includes stringex.h first.
