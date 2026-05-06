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
set(hfc_REVISION 42180f1fb49d5301b1167947335e54ebe76f2573)


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
    set(BOOST_INCLUDE_LIBRARIES "spirit" CACHE STRING "" FORCE)
  ]=]
)

HermeticFetchContent_MakeAvailableAtBuildTime(Boost)


add_executable(spirit_example spirit_example.cpp)
target_link_libraries(spirit_example Boost::headers)

add_executable(spirit_example1 spirit_example1.cpp)
target_link_libraries(spirit_example Boost::headers)

add_executable(spirit_example2 spirit_example2.cpp)
target_link_libraries(spirit_example Boost::headers)

add_executable(spirit_example3 spirit_example3.cpp)
target_link_libraries(spirit_example Boost::headers)

add_executable(spirit_example4 spirit_example4.cpp)
target_link_libraries(spirit_example Boost::headers)

add_executable(spirit_example5 spirit_example5.cpp)
target_link_libraries(spirit_example Boost::headers)