# CMake RE environment for this repository's canonical Linux build.
#
# The reproducibility contract is the image, not this file: everything below names something
# that exists at a fixed path inside tipibuild/tipi-ubuntu-2404:v0.0.87, which is the image the
# devcontainer, the manual `docker run` recipe in CLAUDE.md and the GitHub workflow all use.
# It is the same toolchain environments/monolithic.cmake pins for the splitter's own build, so
# a cmake-re build and a local build compile with the same compiler and the same standard.

if(DEFINED CMAKE_RE_UBUNTU_CLANG_TOOLCHAIN_INCLUDED)
  return()
endif()
set(CMAKE_RE_UBUNTU_CLANG_TOOLCHAIN_INCLUDED TRUE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "The ubuntu-clang CMake RE toolchain requires a Linux host; got '${CMAKE_HOST_SYSTEM_NAME}'.")
endif()

set(CMAKE_C_COMPILER   /usr/local/share/.tipi/clang/4f846ee/bin/clang   CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER /usr/local/share/.tipi/clang/4f846ee/bin/clang++ CACHE PATH "" FORCE)

# C++17 is a build contract here rather than a preference: cpp-splitter can only leave a
# namespace-scope definition in a header that every split piece includes by marking it
# `inline`, which is a C++17 feature. Below it, variables are moved instead and the
# Boost.Spirit test suite fails to link. See environments/monolithic.cmake and TODO/33.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
