# Configures, builds and runs example/cpp-20-modules -- Kitware's C++20 named modules example
# -- with the suite's compiler, plain, and expects `hello world`. TODO/47.
#
# The launcher is not on the build: it does not take module units yet (TODO/43 Phase 0).
# Skips, with the marker the suite maps to a CTest skip, when this CMake is older than 3.28
# or the compiler has no dependency scanner CMake knows how to drive -- clang < 16, Apple's
# clang, GCC < 14 -- which CMake reports at generate time.

foreach(required PROJECTDIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

if(CMAKE_VERSION VERSION_LESS 3.28)
  message("cpp-splitter-test-skip: CMake ${CMAKE_VERSION} has no C++20 module support (needs 3.28)")
  return()
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

set(configure_args -G Ninja -S "${PROJECTDIR}" -B "${WORKDIR}" "-DCMAKE_CXX_COMPILER=${CXX}")
if(DEFINED SYSROOT AND NOT SYSROOT STREQUAL "")
  # A non-Apple clang on macOS does not find the SDK's headers on its own.
  list(APPEND configure_args "-DCMAKE_OSX_SYSROOT=${SYSROOT}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" ${configure_args}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_out
  ERROR_VARIABLE configure_err)
if(NOT configure_result EQUAL 0)
  if("${configure_out}${configure_err}" MATCHES "does not provide a way to discover the import graph")
    message("cpp-splitter-test-skip: ${CXX} has no module dependency scanner CMake can drive")
    return()
  endif()
  message(FATAL_ERROR "configure failed (${configure_result}):\n${configure_out}${configure_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${WORKDIR}" --verbose
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_out
  ERROR_VARIABLE build_err)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "build failed (${build_result}):\n${build_out}${build_err}")
endif()
# What the post promises the build does: scan, then the interface unit compiled as a module
# writing its BMI, then the importer reading it. The module flags sit in a response file
# CMake writes per unit (`@foo.cxx.o.modmap`), so the BMI is looked for there.
if(NOT build_out MATCHES "clang-scan-deps|-fdeps-format=p1689")
  message(FATAL_ERROR "the build did not scan for module dependencies:\n${build_out}")
endif()
set(modmap "${WORKDIR}/CMakeFiles/foo.dir/foo.cxx.o.modmap")
if(NOT EXISTS "${modmap}")
  message(FATAL_ERROR "no module map written for the interface unit at ${modmap}:\n${build_out}")
endif()
file(READ "${modmap}" modmap_text)
if(NOT modmap_text MATCHES "foo\\.(pcm|gcm)")
  message(FATAL_ERROR "the interface unit is not compiled to a BMI:\n${modmap_text}")
endif()

execute_process(
  COMMAND "${WORKDIR}/hello"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_out)
string(STRIP "${run_out}" run_out)
if(NOT run_result EQUAL 0 OR NOT run_out STREQUAL "hello world")
  message(FATAL_ERROR "hello printed '${run_out}' (exit ${run_result}), expected 'hello world'")
endif()
message(STATUS "example/cpp-20-modules: hello world")
