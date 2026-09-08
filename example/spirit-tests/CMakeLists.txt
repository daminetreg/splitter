cmake_minimum_required(VERSION 3.16)
project(spirit_tests CXX)

# Boost.Spirit's own test suite, built by CMake.
#
# Spirit ships no CMakeLists for its tests -- it is header-only and its tests are driven by a
# Boost.Build Jamfile -- so the superproject's BUILD_TESTING produces no Spirit targets at all,
# which is why the other Spirit benchmark measures a consumer instead. These are the real test
# sources from libs/spirit/test, compiled and linked as the programs the Jamfile would build;
# only the driver differs.
#
# Six of them, spread across the four sub-suites, chosen because they build clean and because
# a mix is more representative than six variations of one grammar. The whole suite is 276
# programs and would take many hours to split.
#
# BOOST_ROOT_DIR points at example/boost-to-split. Every library's include directory is added
# because Spirit reaches across most of Boost and listing them by hand goes stale.
if(NOT DEFINED BOOST_ROOT_DIR)
  message(FATAL_ERROR "BOOST_ROOT_DIR must point at the Boost superproject checkout")
endif()

file(GLOB boost_includes "${BOOST_ROOT_DIR}/libs/*/include")
set(spirit_test_dir "${BOOST_ROOT_DIR}/libs/spirit/test")

# Each test includes a test.hpp sitting beside it, so its own directory goes on the path.
function(add_spirit_test suite name)
  add_executable(spirit_test_${suite}_${name} "${spirit_test_dir}/${suite}/${name}.cpp")
  target_include_directories(spirit_test_${suite}_${name} PRIVATE
    "${spirit_test_dir}/${suite}"
    ${boost_includes}
    "${BOOST_ROOT_DIR}/libs/numeric/conversion/include")
  target_compile_features(spirit_test_${suite}_${name} PRIVATE cxx_std_14)
endfunction()

add_spirit_test(qi    char1)
add_spirit_test(qi    alternative)
add_spirit_test(qi    optional)
add_spirit_test(x3    char1)
add_spirit_test(karma char1)
add_spirit_test(lex   lexertl1)
