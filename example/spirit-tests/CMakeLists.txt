cmake_minimum_required(VERSION 3.16)
project(spirit_tests CXX)

# Boost.Spirit's own test suite, built standalone -- this is what the benchmark scripts
# configure.
#
# The Jamfile reader is a module beside this file, shared with the cmake-re build in
# cmake-re/CMakeLists.txt. That build cannot include it from here -- cmake-re mirrors the git
# repository enclosing its source tree, and it is given a directory inside the Boost checkout,
# which has a .git of its own -- so build-spirit-cmake-re.sh copies both into that checkout
# before building. This directory holds the originals.
#
# BOOST_ROOT_DIR points at example/boost-to-split.
if(NOT DEFINED BOOST_ROOT_DIR)
  message(FATAL_ERROR "BOOST_ROOT_DIR must point at the Boost superproject checkout")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/SpiritTestsFromJamfiles.cmake")
