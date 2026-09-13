cmake_minimum_required(VERSION 3.27.7)
project(sample_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The toolchain root is two levels up from the compiler: /usr/local/share/.tipi/clang/<sha1>.
# It holds the libclang the splitter links and, on Linux, the libc++ it is linked against.
get_filename_component(clang_toolchain_root "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(clang_toolchain_root "${clang_toolchain_root}" DIRECTORY)

set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} -stdlib=libc++ ")
if(APPLE)
  # tipi's macOS libclang.dylib is itself linked against the system libc++, so the splitter
  # links the same one and needs nothing static from the toolchain.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")
else()
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -stdlib=libc++ -static-libstdc++ -static-libgcc ${clang_toolchain_root}/lib/libc++.a ${clang_toolchain_root}/lib/libc++abi.a")
endif()

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

# Boost.Context picks its assembly and ABI from CMAKE_SYSTEM_PROCESSOR, which is the processor
# of the cmake process -- x86_64 when tipi's cmake runs under Rosetta -- and not the -arch the
# toolchain compiles for. When a toolchain names one architecture, pass it on.
set(boost_context_architecture "")
if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  set(boost_context_architecture "
    set(BOOST_CONTEXT_ARCHITECTURE arm64 CACHE STRING \"\" FORCE)
    set(BOOST_CONTEXT_ABI aapcs CACHE STRING \"\" FORCE)")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  set(boost_context_architecture "
    set(BOOST_CONTEXT_ARCHITECTURE x86_64 CACHE STRING \"\" FORCE)
    set(BOOST_CONTEXT_ABI sysv CACHE STRING \"\" FORCE)")
endif()

FetchContent_MakeHermetic(
  Boost
  HERMETIC_TOOLCHAIN_EXTENSION
  "
    set(BOOST_BUILD_TEST OFF CACHE BOOL \"\" FORCE)
    set(BOOST_ENABLE_PYTHON OFF CACHE BOOL \"\" FORCE)
    set(BOOST_INCLUDE_LIBRARIES \"process\" CACHE STRING \"\" FORCE)
    ${boost_context_architecture}
  "
)

HermeticFetchContent_MakeAvailableAtBuildTime(Boost)


# Where libclang and its clang-c/ headers come from. The toolchain's own by default, which is
# what every tipi clang provides; a toolchain whose compiler has no libclang headers -- Apple's
# ships the library but not clang-c/ -- names another LLVM here. See environments/.
set(CPP_SPLITTER_LIBCLANG_ROOT "${clang_toolchain_root}" CACHE PATH
    "LLVM install holding lib/libclang and include/clang-c")

add_executable(cpp-splitter src/main.cpp)
target_include_directories(cpp-splitter PRIVATE "${CPP_SPLITTER_LIBCLANG_ROOT}/include")
target_link_libraries(cpp-splitter Boost::headers
  "${CPP_SPLITTER_LIBCLANG_ROOT}/lib/libclang${CMAKE_SHARED_LIBRARY_SUFFIX}")
set_target_properties(cpp-splitter PROPERTIES BUILD_RPATH "${CPP_SPLITTER_LIBCLANG_ROOT}/lib")
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
#   main_in_header_main  `main` defined in an implementation include
#   no_linkage_type_main  a signature mentioning a type from an unnamed namespace
#   extern_c_main       definitions inside extern "C", by macro and by block (TODO/42 1)
#   tail_include_main   an implementation include at the end of the source (TODO/42 1b)
foreach(fixture sample use_mylib static_specifier static_renames member_functions ctx_main
                always_inline_main impl_include_main preamble_linkage macro_members_main
                retracted_macro_main ipp_ctor_main multiline_conditional_main
                trailing_comment_main specialization_member_main static_in_header_main
                macro_argument_extent_main preamble_variables_main macro_definition_main
                main_in_header_main no_linkage_type_main extern_c_main
                tail_include_main)
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

# A `static` variable in the unit's own source: one copy per piece unless it is renamed and
# moved. Two translation units, because the mangle is only load-bearing when both define one.
add_test(
  NAME launcher.static_variable
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          "-DSOURCES=${cpp_splitter_test_dir}/static_variable_main.cpp;${cpp_splitter_test_dir}/static_variable_other.cpp"
          -DINCLUDE_DIR=${cpp_splitter_test_dir}
          -DWORKDIR=${cpp_splitter_test_output}/static_variable
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunLauncherTest.cmake)

# Where a variable that may exist in only one object is put, at C++17 and below it.
add_test(
  NAME launcher.inline_variable_placement
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/inline_variable_main.cpp
          -DWORKDIR=${cpp_splitter_test_output}/inline_variable
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunInlineVariableTest.cmake)

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

# TODO/32 and TODO/33: a variable defined in a header, and an array whose bound comes from its
# initialiser, both have to stay where they are and be marked inline rather than moved.
add_test(
  NAME launcher.variables_stay_in_place
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/header_variable_main.cpp
          -DHEADERDIR=${cpp_splitter_test_dir}/header_variable/pkg
          -DWORKDIR=${cpp_splitter_test_output}/header_variable
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunHeaderVariableTest.cmake)

# Outside the source tree, because cmake-re mirrors the git repository that encloses whatever
# it is pointed at, and this repository is 41176 files.
#
# The suffix keeps two people on one machine out of each other's way. USER is unset in the tipi
# container, which silently produced the shared, surprising "cpp-splitter-remote-split-", so ask
# the system rather than the environment.
execute_process(COMMAND id -u OUTPUT_VARIABLE cpp_splitter_uid OUTPUT_STRIP_TRAILING_WHITESPACE)
set(cpp_splitter_remote_split_workdir "/tmp/cpp-splitter-remote-split-${cpp_splitter_uid}"
    CACHE PATH "Work directory for launcher.remote_split_on_rbe; must be outside any git repo")

# TODO/35: the split itself is produced on the RBE cluster. Talks to a real cluster and skips
# when there are no credentials -- see the driver for why a stub would not be worth having.
add_test(
  NAME launcher.remote_split_on_rbe
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DPROJECTDIR=${cpp_splitter_test_dir}/remote_split
          # Outside the repository on purpose: cmake-re mirrors the enclosing git repository.
          -DWORKDIR=${cpp_splitter_remote_split_workdir}
          -DCXX=${CMAKE_CXX_COMPILER}
          -DTOOLCHAIN=${CMAKE_SOURCE_DIR}/environments/ubuntu-clang.cmake
          -DCMAKE_RE_DIR=${CMAKE_SOURCE_DIR}/cmake-re-dev-latest
          -DSTAGE_DRIVERS=${CMAKE_SOURCE_DIR}/tools/stage-tipi-drivers.sh
          -P ${cpp_splitter_test_dir}/cmake/RunRemoteSplitTest.cmake)
set_tests_properties(launcher.remote_split_on_rbe PROPERTIES
  SKIP_REGULAR_EXPRESSION "cpp-splitter-test-skip:"
  TIMEOUT 900)

# TODO/34: chained behind another compiler launcher, the splitter must probe the real compiler
# for system includes rather than the launcher in front of it.
add_test(
  NAME launcher.chained_behind_driver
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/chained_driver_main.cpp
          -DWORKDIR=${cpp_splitter_test_output}/chained_driver
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunChainedDriverTest.cmake)

# TODO/31: a rewritten header's quoted include must still resolve from inside the mirror.
add_test(
  NAME launcher.quoted_include_in_mirror
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/quoted_include_main.cpp
          -DHEADERDIR=${cpp_splitter_test_dir}/quoted_include/pkg
          -DWORKDIR=${cpp_splitter_test_output}/quoted_include
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunQuotedIncludeTest.cmake)

# TODO/28: an edit confined to one function body is re-sliced from the recorded harvest
# instead of re-parsing the translation unit.
add_test(
  NAME launcher.incremental_body_edit
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          -DSOURCE=${cpp_splitter_test_dir}/incremental_main.cpp
          -DHEADER=${cpp_splitter_test_dir}/incremental_header.hpp
          -DWORKDIR=${cpp_splitter_test_output}/incremental
          -DCXX=${CMAKE_CXX_COMPILER}
          -P ${cpp_splitter_test_dir}/cmake/RunIncrementalSplitTest.cmake)

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

# An explicit function-template specialization whose `inline` arrives through the same macro
# as its always-inline attribute. Two translation units, because the copies have to reach one
# link before the linker can reject them.
add_test(
  NAME launcher.specialization_inline
  COMMAND ${CMAKE_COMMAND}
          -DSPLITTER=$<TARGET_FILE:cpp-splitter>
          "-DSOURCES=${cpp_splitter_test_dir}/specialization_inline_main.cpp;${cpp_splitter_test_dir}/specialization_inline_other.cpp"
          -DINCLUDE_DIR=${cpp_splitter_test_dir}
          -DWORKDIR=${cpp_splitter_test_output}/specialization_inline
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
  split.macro_definition_main split.main_in_header_main split.no_linkage_type_main
  launcher.shared_inline_header launcher.depfile_names_originals
  launcher.explicit_specialization launcher.default_standard launcher.specialization_inline
  launcher.header_edit_behind_pch launcher.inline_variable_placement
  launcher.static_variable
  PROPERTIES TIMEOUT 300)

# The drivers hand ${CXX} to cpp-splitter, which runs it bare: no -isysroot, none of the
# toolchain's CMAKE_CXX_FLAGS. CMake adds those to its own compiles but not to theirs. tipi's
# clang is not Apple's and has no idea where the SDK is unless SDKROOT says so -- without it
# not even <wchar.h> is found -- and the SDK's math.h needs the shim that
# environments/macos-clang.cmake puts on the flags, which clang also takes from
# CPLUS_INCLUDE_PATH. cpp-splitter probes the compiler for its include path, so the libclang
# parse sees the shim the same way the compile does.
if(APPLE)
  set(cpp_splitter_test_environment "SDKROOT=${CMAKE_OSX_SYSROOT}")
  if(DEFINED CMAKE_RE_MACOS_CLANG_SDK_SHIM)
    list(APPEND cpp_splitter_test_environment "CPLUS_INCLUDE_PATH=${CMAKE_RE_MACOS_CLANG_SDK_SHIM}")
  endif()
  get_property(cpp_splitter_all_tests DIRECTORY PROPERTY TESTS)
  set_tests_properties(${cpp_splitter_all_tests} PROPERTIES
    ENVIRONMENT "${cpp_splitter_test_environment}")
endif()
