# CMake RE host environment for building the splitter on macOS.
#
# There is no image here: this is what `cmake-re --host` uses on a Mac, and the contract is the
# clang that tipi installs at a fixed path. /usr/local/share/.tipi/clang/a7e6968 is the sha1 of
# the clang-13-darwin-64bit distro archive, the way 4f846ee is the Linux one's, and the version
# is the same 13.0.0 as in tipibuild/tipi-ubuntu-2404:v0.0.87 -- so the splitter is compiled
# against, and links, the same libclang API on both hosts. The binaries are x86_64; on Apple
# silicon they run under Rosetta.

if(DEFINED CMAKE_RE_MACOS_CLANG_TOOLCHAIN_INCLUDED)
  return()
endif()
set(CMAKE_RE_MACOS_CLANG_TOOLCHAIN_INCLUDED TRUE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "The macos-clang CMake RE toolchain requires a macOS host; got '${CMAKE_HOST_SYSTEM_NAME}'.")
endif()

set(CMAKE_C_COMPILER   /usr/local/share/.tipi/clang/a7e6968/bin/clang   CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER /usr/local/share/.tipi/clang/a7e6968/bin/clang++ CACHE PATH "" FORCE)

# The same contract as environments/ubuntu-clang.cmake: cpp-splitter needs C++17 to leave a
# header-scope variable in place as `inline` rather than move it. See TODO/33.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)

# The current macOS SDK's math.h uses _Float16, which clang 13 does not have on x86_64. The
# shim directory shadows it with a copy that spells the type `float` for that one header; see
# the comment in macos-clang.sdk-shim/math.h. It is a toolchain fact, not a project one: any
# translation unit reaching <cmath> through this compiler needs it.
set(CMAKE_RE_MACOS_CLANG_SDK_SHIM "${CMAKE_CURRENT_LIST_DIR}/macos-clang.sdk-shim")
set(CMAKE_CXX_FLAGS_INIT "-isystem ${CMAKE_RE_MACOS_CLANG_SDK_SHIM}")
set(CMAKE_C_FLAGS_INIT   "-isystem ${CMAKE_RE_MACOS_CLANG_SDK_SHIM}")
