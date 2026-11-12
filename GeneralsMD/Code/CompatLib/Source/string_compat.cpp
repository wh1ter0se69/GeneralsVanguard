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

#include "string_compat.h"

#include <sstream>
#include <streambuf>
#include <ostream>

// Do not write through std::stringbuf::pubsetbuf: it is implementation-defined (libc++ ignores it), so on macOS str was
// never written. OpenContain::exitObjectViaDoor builds the exit bone name from itoa(m_whichExitPath); the failed lookup
// silently put the passenger at the container centre: a CRC divergence. Caller sizes the buffer; sign only in base 10;
// NUL-terminated.
char* itoa(int value, char* str, int base)
{
  if (str == nullptr)
    return str;

  if (base < 2 || base > 36)
  {
    str[0] = '\0';
    return str;
  }

  // Build digits least-significant first into a scratch buffer. 32 digits covers base 2 for a
  // 32-bit value, plus sign and terminator.
  char tmp[35];
  int n = 0;

  // Only base 10 is signed, as MSVC's _itoa does. Go through unsigned so INT_MIN cannot overflow.
  const bool negative = (value < 0 && base == 10);
  unsigned int uvalue = negative
    ? (unsigned int)(0u - (unsigned int)value)
    : (unsigned int)value;

  if (uvalue == 0u)
  {
    tmp[n++] = '0';
  }
  else
  {
    while (uvalue != 0u)
    {
      const unsigned int digit = uvalue % (unsigned int)base;
      tmp[n++] = (char)(digit < 10u ? ('0' + digit) : ('a' + digit - 10u));
      uvalue /= (unsigned int)base;
    }
  }

  int out = 0;
  if (negative)
    str[out++] = '-';
  while (n > 0)
    str[out++] = tmp[--n];
  str[out] = '\0';

  return str;
}

int _vsnwprintf(wchar_t* buffer, size_t count, const wchar_t* format, va_list args)
{
  std::wstring format_fixup(format);

  // Replace all %s with %ls
  size_t pos = format_fixup.find(L"%s", 0);
  while (pos != std::wstring::npos)
  {
    format_fixup.replace(pos, 2, L"%ls");
    pos += 3;
    pos = format_fixup.find(L"%s", pos);
  }

  // Replace all %S with %s
  pos = format_fixup.find(L"%S", 0);
  while (pos != std::wstring::npos)
  {
    format_fixup.replace(pos, 2, L"%s");
    pos += 2;
    pos = format_fixup.find(L"%S", pos);
  }


  return vswprintf(buffer, count, format_fixup.c_str(), args);
}

// Also defined in GameSpy gsplatformutil
__attribute__((weak))
char* _strlwr(char* str)
{
  for (int i = 0; str[i] != '\0'; i++)
  {
    str[i] = tolower(str[i]);
  }
  return str;
}

// GeneralsX @build fbraz 11/02/2026 - Linux portability: uppercase string
__attribute__((weak))
char* _strupr(char* str)
{
  for (int i = 0; str[i] != '\0'; i++)
  {
    str[i] = toupper(str[i]);
  }
  return str;
}