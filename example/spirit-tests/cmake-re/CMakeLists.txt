cmake_minimum_required(VERSION 3.16)
project(boost_spirit_cmake_re CXX)

# Boost.Spirit and its own test suite, as one CMake RE build.
#
# Two things get built and they answer different questions:
#
#   the superproject   configured for Spirit. Spirit is header-only, so what this produces is
#                      Spirit's interface target plus the compiled libraries it and its tests
#                      depend on.
#   the test suite     the 277 programs SpiritTestsFromJamfiles.cmake reads out of Spirit's
#                      Boost.Build Jamfiles, which is where the compilation work is.
#
# This file and SpiritTestsFromJamfiles.cmake beside it are *copies*. The originals live in
# example/spirit-tests/, and build-spirit-cmake-re.sh installs them into the Boost checkout
# before invoking cmake-re. Editing the copy is editing something the next run overwrites.
#
# The copying is not tidiness. cmake-re mirrors the git repository enclosing the source tree it
# is given, and this checkout has a .git of its own, so a project here can reach nothing in the
# cpp-splitter repository around it -- an earlier version tried and configure failed on a path
# that had resolved outside the mirror. The same boundary stops cpp-splitter from tracking
# files here at all, which is why the originals live elsewhere.

get_filename_component(boost_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set(BOOST_INCLUDE_LIBRARIES "spirit" CACHE STRING "" FORCE)
set(BOOST_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
# The suite added below *is* the test suite. Letting the superproject add its own would build a
# second and much larger one that this build has no opinion about.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory("${boost_root}" boost EXCLUDE_FROM_ALL)

set(BOOST_ROOT_DIR "${boost_root}")
include("${CMAKE_CURRENT_SOURCE_DIR}/SpiritTestsFromJamfiles.cmake")
