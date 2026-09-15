# CMake RE host environment for building the splitter with Homebrew's LLVM as both the
# compiler it drives and the libclang it parses with. TODO/43.
#
# What C++20 named modules need that Apple's clang does not have: `clang-scan-deps` beside
# the compiler for CMake's dependency scan, `-fmodule-output=` for the BMI, and
# `-fmodules-reduced-bmi` (clang >= 20). Homebrew's llvm has all three, and a libclang of
# the same version -- which matters here more than usual, since libclang has to load the
# BMIs the compiler wrote when it parses an importer.
#
# A non-Apple clang does not find the SDK on its own, hence the sysroot.

if(DEFINED CMAKE_RE_MACOS_BREW_LLVM_TOOLCHAIN_INCLUDED)
  return()
endif()
set(CMAKE_RE_MACOS_BREW_LLVM_TOOLCHAIN_INCLUDED TRUE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "The macos-brew-llvm CMake RE toolchain requires a macOS host; got '${CMAKE_HOST_SYSTEM_NAME}'.")
endif()

execute_process(COMMAND brew --prefix llvm
                OUTPUT_VARIABLE homebrew_llvm OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE homebrew_llvm_result ERROR_QUIET)
if(NOT homebrew_llvm_result EQUAL 0 OR NOT EXISTS "${homebrew_llvm}/bin/clang++")
  message(FATAL_ERROR
    "The macos-brew-llvm toolchain needs Homebrew's llvm: `brew install llvm` (got '${homebrew_llvm}').")
endif()

set(CMAKE_C_COMPILER   "${homebrew_llvm}/bin/clang"   CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER "${homebrew_llvm}/bin/clang++" CACHE PATH "" FORCE)
set(CPP_SPLITTER_LIBCLANG_ROOT "${homebrew_llvm}" CACHE PATH
    "LLVM install holding lib/libclang and include/clang-c" FORCE)

execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(CMAKE_OSX_SYSROOT "${macos_sdk}" CACHE PATH "" FORCE)

# The same contract as the other environments: cpp-splitter needs C++17 to leave a
# header-scope variable in place as `inline` rather than move it. See TODO/33.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)

execute_process(COMMAND sysctl -n hw.optional.arm64
                OUTPUT_VARIABLE macos_has_arm64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(macos_has_arm64 STREQUAL "1")
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
else()
  set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "" FORCE)
endif()
