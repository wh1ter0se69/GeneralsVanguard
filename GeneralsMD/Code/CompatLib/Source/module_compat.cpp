#include "types_compat.h"
#include "module_compat.h"

#include <unistd.h>
#include <dlfcn.h>
#include <string.h>

// GeneralsX @feature BenderAI 24/02/2026 Phase 5 macOS executable path detection
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// GeneralsX @bugfix 10/09/2026 readlink() neither NUL-terminates nor stops short of the
// buffer: it returns up to `size` bytes, so the old `buffer[count] = 0` wrote one byte past
// a full buffer. Every caller passes the whole array (sizeof(buf) / ARRAY_SIZE(buf)), so a
// path at or over the limit - 1024 here, see _MAX_PATH in windows_compat.h - overflowed the
// caller's stack. Truncation is reported as failure rather than returned as a short path,
// because this shim returns Bool and cannot carry Win32's length-on-truncation contract.
// The buffer is cleared on every failure path: several callers ignore the return value and
// then read the buffer as a string (GameMemoryInit.cpp, Debug.cpp, MiniLog.cpp,
// Generals/GlobalData.cpp), which without this would be an out-of-bounds read.
bool GetModuleFileName(HINSTANCE hInstance, char* buffer, int size)
{
  if (buffer == nullptr || size <= 0)
    return false;

#ifdef __linux__
  // A negative `size` would become a huge size_t here, hence the guard above.
  ssize_t count = readlink("/proc/self/exe", buffer, static_cast<size_t>(size));
  if (count < 0 || count >= size) {
    buffer[0] = '\0';
    return false;
  }
  buffer[count] = '\0';
  return true;
#elif defined(__APPLE__)
  // Already bounds-correct: _NSGetExecutablePath takes bufsize in/out and returns -1 when
  // the buffer is too small, so it never writes past the end.
  uint32_t bufsize = (uint32_t)size;
  int ret = _NSGetExecutablePath(buffer, &bufsize);
  if (ret == 0) {
    return true;
  }
  buffer[0] = '\0';
  return false;
#else
  buffer[0] = '\0';
  return false;
#endif
}

HMODULE LoadLibrary(const char* lpFileName)
{
  // GeneralsX @feature BenderAI 24/02/2026 Phase 5 - Platform-agnostic library loading without C++17
  // Check if filename ends with ".dll" and remove extension if so
  const char* ext = strrchr(lpFileName, '.');
  if (ext && strcmp(ext, ".dll") == 0) {
    // Create a copy without the .dll extension
    size_t len = ext - lpFileName;
    char pathCurrent[512];
    if (len < sizeof(pathCurrent)) {
      strncpy(pathCurrent, lpFileName, len);
      pathCurrent[len] = '\0';
      return dlopen(pathCurrent, RTLD_LAZY);
    }
  }
  
  return dlopen(lpFileName, RTLD_LAZY);
}

FARPROC GetProcAddress(HMODULE hModule, const char* lpProcName)
{
  return (FARPROC)dlsym(hModule, lpProcName);
}

void FreeLibrary(HMODULE hModule)
{
  // Currently misbehaves by unmapping regions not related to the library
  // dlclose(hModule);
}