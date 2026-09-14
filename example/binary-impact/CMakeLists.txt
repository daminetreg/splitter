# The use_mylib fixture of the test suite as a project: one source, one header of inline
# functions and a function template. compare.sh configures this project five times -- plain,
# plain + LTO, and split under three relocatable linkers -- and compares what comes out.
# TODO/46.
cmake_minimum_required(VERSION 3.20)
project(binary_impact CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BINARY_IMPACT_LTO "Compile and link with -flto=thin" OFF)

add_executable(use_mylib use_mylib.cpp)
target_include_directories(use_mylib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# lld for every configuration, so the linker is not a variable: GNU ld cannot read the
# bitcode objects the LTO configurations produce.
target_link_options(use_mylib PRIVATE -fuse-ld=lld)
if(BINARY_IMPACT_LTO)
  target_compile_options(use_mylib PRIVATE -flto=thin)
  target_link_options(use_mylib PRIVATE -flto=thin)
endif()
