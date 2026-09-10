cmake_minimum_required(VERSION 3.16)
project(remote_split_probe CXX)

# Two translation units behind one shared header: enough for the split to produce several
# pieces per unit and for a header to have to reach the worker, small enough that the test
# builds it twice in a few seconds.
add_library(rsp STATIC a.cpp b.cpp)
target_include_directories(rsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(rsp PRIVATE cxx_std_17)
