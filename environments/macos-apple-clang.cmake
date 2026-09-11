# CMake RE host environment for building the splitter with the clang macOS ships.
#
# What a GitHub macOS runner has: Apple's clang from the Xcode command line tools, and
# Homebrew. The cmake-re installer does not bring tipi's clang there, so this toolchain takes
# the system compiler instead of environments/macos-clang.cmake's fixed path. Apple's toolchain
# ships libclang.dylib but not the clang-c/ headers the splitter is written against, so those
# and the library come from Homebrew's llvm (`brew install llvm`), which has both.
#
# The compiler the splitter drives and the libclang it parses with are therefore two LLVMs.
# That is the launcher's normal situation -- libclang is whatever the splitter was linked
# against, the driver is whatever the build uses -- and the splitter probes the driver for its
# include path rather than assuming its own.

if(DEFINED CMAKE_RE_MACOS_APPLE_CLANG_TOOLCHAIN_INCLUDED)
  return()
endif()
set(CMAKE_RE_MACOS_APPLE_CLANG_TOOLCHAIN_INCLUDED TRUE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "The macos-apple-clang CMake RE toolchain requires a macOS host; got '${CMAKE_HOST_SYSTEM_NAME}'.")
endif()

set(CMAKE_C_COMPILER   /usr/bin/clang   CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE PATH "" FORCE)

# The same contract as the other environments: cpp-splitter needs C++17 to leave a
# header-scope variable in place as `inline` rather than move it. See TODO/33.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)

# tipi's cmake is an x86_64 binary and runs under Rosetta on Apple silicon, and a translated
# cmake defaults the build to its own architecture -- `-arch x86_64` on a machine whose
# Homebrew libclang is arm64. Ask the hardware rather than the (translated) process: `uname -m`
# says x86_64 from under Rosetta, hw.optional.arm64 does not lie.
execute_process(COMMAND sysctl -n hw.optional.arm64
                OUTPUT_VARIABLE macos_has_arm64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(macos_has_arm64 STREQUAL "1")
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
else()
  set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "" FORCE)
endif()

# Homebrew's prefix depends on the machine (/opt/homebrew on Apple silicon, /usr/local on
# Intel), so ask it. Cached so the answer is the same on every re-read of this file.
if(NOT DEFINED CPP_SPLITTER_LIBCLANG_ROOT)
  execute_process(COMMAND brew --prefix llvm
                  OUTPUT_VARIABLE homebrew_llvm OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE homebrew_llvm_result ERROR_QUIET)
  if(NOT homebrew_llvm_result EQUAL 0 OR NOT EXISTS "${homebrew_llvm}/include/clang-c/Index.h")
    message(FATAL_ERROR
      "The macos-apple-clang toolchain needs Homebrew's llvm for libclang and clang-c/: "
      "`brew install llvm` (got '${homebrew_llvm}').")
  endif()
  set(CPP_SPLITTER_LIBCLANG_ROOT "${homebrew_llvm}" CACHE PATH
      "LLVM install holding lib/libclang and include/clang-c" FORCE)
endif()
