/* Lua 5.4.9 (https://www.lua.org, MIT, see LICENSE) as one C++ translation unit (errors unwind as C++
   exceptions), without the standalone interpreter/compiler and without io/os/package/debug libs, so
   scripts are sandboxed by construction. Compiled by CMake for DoS-Tool.dll; #included by native tests
   (after all std headers: the private headers define macros such as `next`). */
#define LUA_CORE
#define LUA_LIB
#include "src/lapi.c"
#include "src/lcode.c"
#include "src/lctype.c"
#include "src/ldebug.c"
#include "src/ldo.c"
#include "src/ldump.c"
#include "src/lfunc.c"
#include "src/lgc.c"
#include "src/llex.c"
#include "src/lmem.c"
#include "src/lobject.c"
#include "src/lopcodes.c"
#include "src/lparser.c"
#include "src/lstate.c"
#include "src/lstring.c"
#include "src/ltable.c"
#include "src/ltm.c"
#include "src/lundump.c"
#include "src/lvm.c"
#include "src/lzio.c"
#include "src/lauxlib.c"
#include "src/lbaselib.c"
#include "src/lcorolib.c"
#include "src/lmathlib.c"
#include "src/lstrlib.c"
#include "src/ltablib.c"
#include "src/lutf8lib.c"
