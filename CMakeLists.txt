cmake_minimum_required(VERSION 3.27.7)
project(sample_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -stdlib=libc++ ")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -stdlib=libc++ -static-libstdc++ -static-libgcc /usr/local/share/.tipi/clang/4f846ee/lib/libc++.a /usr/local/share/.tipi/clang/4f846ee/lib/libc++abi.a")

#add_executable(sample_app sample.cpp)


set(FETCHCONTENT_QUIET OFF)
include(FetchContent)


set(hfc_REPOSITORY https://github.com/tipi-build/hfc)
set(hfc_REVISION 9d56120886b03b28b04e5ee1c8a8868918cc3f51)


set(HERMETIC_FETCHCONTENT_FORWARDED_CMAKE_VARIABLES
  "CMAKE_PREFIX_PATH"
  "CMAKE_BUILD_TYPE"
  "CMAKE_DEBUG_POSTFIX"
  "CMAKE_EXE_LINKER_FLAGS"
  "CMAKE_C_COMPILER"
  "CMAKE_C_FLAGS"
  "CMAKE_C_COMPILE_FEATURES"
  "CMAKE_C_EXTENSIONS"
  "CMAKE_C_STANDARD"
  "CMAKE_C_STANDARD_REQUIRED"
  "CMAKE_CXX_COMPILER"
  "CMAKE_CXX_FLAGS"
  "CMAKE_CXX_COMPILE_FEATURES"
  "CMAKE_CXX_COMPILER_IMPORT_STD"
  "CMAKE_CXX_EXTENSIONS"
  "CMAKE_CXX_SCAN_FOR_MODULES"
  "CMAKE_CXX_STANDARD"
  "CMAKE_CXX_STANDARD_REQUIRED"
  "BUILD_SHARED_LIBS"
  "FORCE_SYSTEM_Iconv"
  "OPENSSL_ROOT_DIR"
  "OPENSSL_INCLUDE_DIR"
)

include(HermeticFetchContent OPTIONAL RESULT_VARIABLE hfc_included) 
if(NOT hfc_included)
  FetchContent_Populate(
    hfc
    GIT_REPOSITORY https://github.com/tipi-build/hfc.git
    GIT_TAG ${hfc_REVISION}
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/src"
    SUBBUILD_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/subbuild"
    BINARY_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache/hfc/bin"
  )
  FetchContent_GetProperties(hfc)
  message(STATUS "Hermetic FetchContent ${hfc_REVISION} available at '${hfc_SOURCE_DIR}'")
  list(APPEND CMAKE_MODULE_PATH "${hfc_SOURCE_DIR}/cmake")
  include(HermeticFetchContent) 
endif()

FetchContent_Declare(
  Boost
  GIT_REPOSITORY https://github.com/boostorg/boost
  GIT_TAG        "ab7968a0bbcf574a7859240d1d8443f58ed6f6cf" # 1.85
)

FetchContent_MakeHermetic(
  Boost
  HERMETIC_TOOLCHAIN_EXTENSION
  [=[
    set(BOOST_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(BOOST_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
    set(BOOST_INCLUDE_LIBRARIES "process" CACHE STRING "" FORCE)
  ]=]
)

HermeticFetchContent_MakeAvailableAtBuildTime(Boost)


get_filename_component(clang_toolchain_root "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(clang_toolchain_root "${clang_toolchain_root}" DIRECTORY)

add_executable(cpp-splitter src/main.cpp)
target_include_directories(cpp-splitter PRIVATE "${clang_toolchain_root}/include")
target_link_libraries(cpp-splitter Boost::headers "${clang_toolchain_root}/lib/libclang.so")
set_target_properties(cpp-splitter PROPERTIES BUILD_RPATH "${clang_toolchain_root}/lib")
# --- Tests ----------------------------------------------------------------------------
#
# Each test splits a fixture in test/, compiles the pieces, links them and runs the result.
# The fixtures are written to return 0 only when the split code computes what the original
# would have, so a wrong split shows up as a failing program rather than as output nobody
# reads. The drivers additionally fail the test if cpp-splitter fell back to compiling the
# source whole, which would otherwise produce a passing program that tested nothing.

enable_testing()

set(cpp_splitter_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/test")
set(cpp_splitter_test_output "${CMAKE_CURRENT_BINARY_DIR}/test-output")

# Fixtures split, compiled and linked on their own through the command line interface.
#   sample            free functions, class methods and namespaces
#   use_mylib         a source whose sibling header is split alongside it
#   static_specifier  internal linkage without the `static` keyword, next to `static_cast`
#   static_renames    a renamed internal-linkage function used from namespace scope
#   member_functions  members written out of line, and class templates kept in the header
#   ctx_main          a header that only compiles in the context of its includer
foreach(fixture sample use_mylib static_specifier static_renames member_functions ctx_main)
  add_test(
    NAME split.${fixture}
    COMMAND ${CMAKE_COMMAND}
            -DSPLITTER=$<TARGET_FILE:cpp-splitter>
            -DSOURCE=${cpp_splitter_test_dir}/${fixture}.cpp
            -DWORKDIR=${cpp_splitter_test_output}/${fixture}
            -DCXX=${CMAKE_CXX_COMPILER}
            -P ${cpp_splitter_test_dir}/cmake/RunSplitTest.cmake)
endforeach()

# One header split separately by two translation units: both objects carry a copy of its
# functions, so the final link has to merge them instead of rejecting them as duplicates.
add_test(
  NAME launcher.shared_inline_header
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          "-DSOURCES=${cpp_splitter_test_dir}/inline_a.cpp;${cpp_splitter_test_dir}/inline_b.cpp"
          -DINCLUDE_DIR=${cpp_splitter_test_dir}
          -DWORKDIR=${cpp_splitter_test_output}/shared_inline_header
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunLauncherTest.cmake)

set_tests_properties(
  split.sample split.use_mylib split.static_specifier split.static_renames
  split.member_functions split.ctx_main launcher.shared_inline_header
  PROPERTIES TIMEOUT 300)
