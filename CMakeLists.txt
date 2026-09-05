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
#   always_inline_main  an always-inline function split out of a header, and a call to it
#   impl_include_main   a unit whose whole body arrives through an implementation include
#   preamble_linkage    a virtual definition kept in the preamble, which may exist once only
#   macro_members_main  a class whose members are produced by one macro expansion
#   retracted_macro_main  a header that retracts the macros its own conditionals test
#   ipp_ctor_main       a constructor defined in an implementation include
#   multiline_conditional_main  a definition under a condition continued across lines
#   trailing_comment_main  a declarator whose last line is a `//` comment
#   specialization_member_main  a member of a full class-template specialization
#   static_in_header_main  a static function in one header, called from another
#   macro_argument_extent_main  one macro, two overlapping but unequal extents
#   preamble_variables_main  variable definitions carried into every piece
#   macro_definition_main  a macro expanding to one out-of-line member definition
foreach(fixture sample use_mylib static_specifier static_renames member_functions ctx_main
                always_inline_main impl_include_main preamble_linkage macro_members_main
                retracted_macro_main ipp_ctor_main multiline_conditional_main
                trailing_comment_main specialization_member_main static_in_header_main
                macro_argument_extent_main preamble_variables_main macro_definition_main)
  add_test(
    NAME split.${fixture}
    COMMAND ${CMAKE_COMMAND}
            -DSPLITTER=$<TARGET_FILE:cpp-splitter>
            -DSOURCE=${cpp_splitter_test_dir}/${fixture}.cpp
            -DWORKDIR=${cpp_splitter_test_output}/${fixture}
            -DCXX=${CMAKE_CXX_COMPILER}
            -P ${cpp_splitter_test_dir}/cmake/RunSplitTest.cmake)
endforeach()

# What the launcher tells the build system a split object depends on. The pieces include
# rewritten copies of the headers, so without help the depfile names only those and an edit
# to the real header rebuilds nothing.
add_test(
  NAME launcher.depfile_names_originals
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/depfile_main.cpp
          -DHEADER=${cpp_splitter_test_dir}/depfile_header.hpp
          -DWORKDIR=${cpp_splitter_test_output}/depfile
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunDepfileTest.cmake)

# A header edited behind the prefix PCH. Needs a driver of its own because the test is what
# the *second* split does, which no single-shot driver can express.
add_test(
  NAME launcher.header_edit_behind_pch
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/header_edit_main.cpp
          -DHEADER=${cpp_splitter_test_dir}/header_edit_header.hpp
          -DWORKDIR=${cpp_splitter_test_output}/header_edit
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunHeaderEditTest.cmake)

# What language standard libclang parses at when the command line names none. Its own driver
# is needed because the check is that split and plain agree, and because no -std may appear
# on the command line -- which is exactly what the other fixtures' driver passes.
add_test(
  NAME launcher.default_standard
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/default_standard_main.cpp
          -DWORKDIR=${cpp_splitter_test_output}/default_standard
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunLauncherStdTest.cmake)

# An explicit specialization of a member of a class template, in a source that defines
# nothing else. Two translation units, because the caller has to be a separate object for
# the specialization to be needed as a symbol rather than inlined.
add_test(
  NAME launcher.explicit_specialization
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          "-DSOURCES=${cpp_splitter_test_dir}/spec_member_impl.cpp;${cpp_splitter_test_dir}/spec_member_main.cpp"
          -DINCLUDE_DIR=${cpp_splitter_test_dir}
          -DWORKDIR=${cpp_splitter_test_output}/explicit_specialization
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunLauncherTest.cmake)

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
  split.member_functions split.ctx_main split.always_inline_main split.impl_include_main
  split.preamble_linkage split.macro_members_main split.retracted_macro_main
  split.ipp_ctor_main split.multiline_conditional_main split.trailing_comment_main
  split.specialization_member_main split.static_in_header_main
  split.macro_argument_extent_main split.preamble_variables_main
  split.macro_definition_main
  launcher.shared_inline_header launcher.depfile_names_originals
  launcher.explicit_specialization launcher.default_standard
  launcher.header_edit_behind_pch
  PROPERTIES TIMEOUT 300)
