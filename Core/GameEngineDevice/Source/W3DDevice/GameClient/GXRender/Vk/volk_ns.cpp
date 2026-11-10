// GeneralsX: iOS only (CMakeLists.txt): volk compiled as C++ under VOLK_NAMESPACE. The iOS game
// force-loads the static libMoltenVK.a (cmake/config-build.cmake), whose vkCreateInstance / vkGetInstanceProcAddr / ... are global
// FUNCTIONS; C volk defines the same names as global function-pointer VARIABLES - one image, two definitions. In namespace volk the
// pointers get C++ names and cannot collide; volk.h's `using namespace volk` keeps every call site unchanged.
#include "volk.c"
