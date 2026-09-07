cmake_minimum_required(VERSION 3.16)
project(static_init_order CXX)

# Two translation units, so the registrar and the variable it reads can land in different
# objects. Nothing here depends on Boost.
add_executable(static_init_order registry.cpp main.cpp)
target_include_directories(static_init_order PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
