cmake_minimum_required(VERSION 3.16)
project(geometry_bench CXX)

# Four Boost.Geometry translation units and a driver, all behind one shared header. Enough
# units for an incremental edit to have somewhere to be incremental, few enough that a cold
# build of the split tree finishes in minutes rather than hours.
#
# BOOST_ROOT_DIR points at example/boost-to-split; every library's include directory is added
# because Geometry reaches across most of Boost and listing them by hand goes stale.
if(NOT DEFINED BOOST_ROOT_DIR)
  message(FATAL_ERROR "BOOST_ROOT_DIR must point at the Boost superproject checkout")
endif()

file(GLOB boost_includes "${BOOST_ROOT_DIR}/libs/*/include")

add_executable(geometry_bench
  area.cpp
  distance.cpp
  overlay.cpp
  hull.cpp
  main.cpp)

target_include_directories(geometry_bench PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${boost_includes}
  "${BOOST_ROOT_DIR}/libs/numeric/conversion/include")
