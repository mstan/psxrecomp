# Cross-compile toolchain: Linux host → Windows x86_64 (MinGW-w64).
#
# Usage from a game repo:
#   cmake -DCMAKE_TOOLCHAIN_FILE=psxrecomp/cmake/toolchain-mingw-w64.cmake ...
#
# Requires (Arch / CachyOS packages):
#   mingw-w64-gcc  mingw-w64-sdl2  (and cmake, ninja)
# Debian/Ubuntu equivalents:
#   g++-mingw-w64-x86-64  + a MinGW SDL2 (distro or self-built)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW target triple")

set(CMAKE_C_COMPILER   "${MINGW_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${MINGW_TRIPLE}-g++")
set(CMAKE_RC_COMPILER  "${MINGW_TRIPLE}-windres")
set(CMAKE_RANLIB       "${MINGW_TRIPLE}-ranlib")
set(CMAKE_AR           "${MINGW_TRIPLE}-ar")
set(CMAKE_STRIP        "${MINGW_TRIPLE}-strip")

# Prefer the triple-prefixed pkg-config so we do not pick up host Linux SDL2.
find_program(_MINGW_PKG_CONFIG NAMES "${MINGW_TRIPLE}-pkg-config" pkg-config)
if(_MINGW_PKG_CONFIG)
  set(PKG_CONFIG_EXECUTABLE "${_MINGW_PKG_CONFIG}" CACHE FILEPATH
      "pkg-config for MinGW sysroot" FORCE)
endif()

# Isolate pkg-config from the host's .pc files.
set(ENV{PKG_CONFIG_PATH} "")
if(DEFINED ENV{MINGW_PKG_CONFIG_LIBDIR} AND NOT "$ENV{MINGW_PKG_CONFIG_LIBDIR}" STREQUAL "")
  set(ENV{PKG_CONFIG_LIBDIR} "$ENV{MINGW_PKG_CONFIG_LIBDIR}")
elseif(EXISTS "/usr/${MINGW_TRIPLE}/lib/pkgconfig")
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/${MINGW_TRIPLE}/lib/pkgconfig")
endif()

set(CMAKE_FIND_ROOT_PATH "/usr/${MINGW_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Large generated TUs can exceed COFF section limits on older binutils.
string(APPEND CMAKE_C_FLAGS_INIT " -Wa,-mbig-obj")
string(APPEND CMAKE_CXX_FLAGS_INIT " -Wa,-mbig-obj")
