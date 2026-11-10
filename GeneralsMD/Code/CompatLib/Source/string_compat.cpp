#include "string_compat.h"

#include <sstream>
#include <streambuf>
#include <ostream>

// GeneralsX: THIS FUNCTION WROTE NOTHING AT ALL ON macOS, AND IT CAUSED A
// CROSS-PLATFORM SIMULATION DIVERGENCE.
//
// The previous body handed `str` to std::stringbuf::pubsetbuf and streamed into it. pubsetbuf on a
// stringbuf is IMPLEMENTATION-DEFINED: libstdc++ honours it, libc++ IGNORES it completely and the
// stringbuf keeps using its own internal buffer. So on macOS `str` was never touched and every
// caller read whatever happened to be on the stack.
//
// Measured, minimal repro compiled on both toolchains with the buffer poisoned to 0xAB first:
//   macOS   / libc++    : AB AB AB AB AB AB AB AB   - never written
//   linux   / libstdc++ : 32 00 AB AB AB AB AB AB   - "2", correct
//
// HOW THAT BECAME A DESYNC. OpenContain::exitObjectViaDoor builds an exit bone name for a container
// with NumberOfExitPaths > 1 as "ExitStart" + '0' + itoa(m_whichExitPath). On macOS the suffix was
// stack garbage, so it asked the model for a bone called "ExitStart0\xAB\xB5..." which does not
// exist. Object::getSingleLogicalBonePosition then takes its SILENT fallback
// (Object.cpp:6161, *position = *getPosition()) and the passenger is placed at the container's own
// centre instead of at the door - 13.2 world units away from where every x86 peer put it. That is
// a hard CRC divergence, and it took a full replay bisect to find because nothing anywhere reports
// a failed bone lookup.
//
// Nothing about this was 60 Hz specific. It fires on any tick rate, on any container with more
// than one exit path. The 30 Hz soaks only looked clean because the AI never unloaded one.
//
// The old body was wrong three ways: it ignored `base` entirely (always decimal), it hard-coded a
// 33-byte capacity while callers pass buffers as small as char[8] (OpenContain.cpp), and it relied
// on implementation-defined behaviour for the write itself.
//
// This replacement follows the traditional itoa contract: caller guarantees the buffer is large
// enough, negative values are only sign-prefixed for base 10 (matching MSVC's _itoa, which treats
// the value as unsigned for every other base), and the result is always NUL-terminated.
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