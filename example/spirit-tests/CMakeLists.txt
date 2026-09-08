cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0007 NEW)
project(spirit_tests CXX)

# Boost.Spirit's own test suite, built by CMake.
#
# Spirit ships no CMakeLists for its tests: it is header-only and its tests are driven by a
# Boost.Build Jamfile, so the superproject's BUILD_TESTING produces no Spirit targets at all.
# That is why the other Spirit benchmark measures a consumer instead.
#
# The targets here are read out of those Jamfiles rather than listed by hand, so this cannot
# drift from the suite it claims to be. Each sub-suite's Jamfile states its tests one per line
# in one of three forms:
#
#     run actions.cpp ;                  -> build and link a program
#     compile pass_container3.cpp ;      -> compile only, never linked
#     compile-fail grammar_fail.cpp ;    -> must NOT compile
#
# `run` becomes an executable and `compile` an object library. `compile-fail` is deliberately
# not represented: those four sources are supposed to fail, and a build that is expected to
# fail cannot be timed against one that is not.
#
# BOOST_ROOT_DIR points at example/boost-to-split. Every library's include directory is added
# because Spirit reaches across most of Boost and listing them by hand goes stale.
if(NOT DEFINED BOOST_ROOT_DIR)
  message(FATAL_ERROR "BOOST_ROOT_DIR must point at the Boost superproject checkout")
endif()

file(GLOB boost_includes "${BOOST_ROOT_DIR}/libs/*/include")
set(spirit_test_dir "${BOOST_ROOT_DIR}/libs/spirit/test")

# Tests excluded from the port, by "<suite>/<name>", each with the reason it is out. Anything
# here is a limitation of this harness or of the toolchain, never of the splitter -- the point
# of the benchmark is that both trees build the same set.
#
# Kept as data rather than commented-out lines so that the count below is honest: the summary
# reports how many the Jamfiles declare and how many are built.
set(SPIRIT_TESTS_EXCLUDE
  # filled in from a plain build; see the message() at the end for the running count
)

set(declared 0)
set(built 0)
set(skipped "")

# `<include>.` in every sub-suite's project requirements: a test includes the test.hpp sitting
# beside it.
function(add_spirit_test kind suite name extra_source)
  set(sources "${spirit_test_dir}/${suite}/${name}.cpp")
  if(NOT EXISTS "${sources}")
    return()
  endif()
  if(NOT extra_source STREQUAL "")
    list(APPEND sources "${extra_source}")
  endif()
  set(target "spirit_test_${suite}_${name}")
  if(kind STREQUAL "run")
    add_executable(${target} ${sources})
  else()
    add_library(${target} OBJECT ${sources})
  endif()
  target_include_directories(${target} PRIVATE
    "${spirit_test_dir}/${suite}"
    ${boost_includes}
    "${BOOST_ROOT_DIR}/libs/numeric/conversion/include")
  target_compile_features(${target} PRIVATE cxx_std_14)
endfunction()

foreach(suite qi karma lex x3 support)
  set(jamfile "${spirit_test_dir}/${suite}/Jamfile")
  if(NOT EXISTS "${jamfile}")
    message(WARNING "no Jamfile for ${suite}")
    continue()
  endif()

  # Filtered at read time, and the filter is what makes this work at all.
  #
  # A Jamfile ends every statement with `;`, which is CMake's list separator, and x3/Jamfile
  # additionally carries backslashes inside comments (`grep -Er "\\("`). Read whole, the two
  # together corrupt the list: a backslash escapes the separator after it and the rest of the
  # file arrives as one element. CMake's regex does not help either -- `[^\n]` is not a
  # newline class to it. Every one of x3's 61 tests vanished silently that way, and the port
  # reported a confident 216 of 277.
  #
  # Passing REGEX to file(STRINGS) keeps the offending lines out of the list in the first
  # place: the ones that survive are statements with no backslash and a single trailing `;`,
  # which CMake escapes correctly.
  file(STRINGS "${jamfile}" jam_lines REGEX "^[ \t]*(run|compile|obj)[ \t]+")

  # `obj <name> : <source>.cpp` declares a translation unit that a later `run` links against.
  # Collected first so that the `run` lines below can find them whatever the order.
  set(obj_names "")
  set(obj_sources "")
  foreach(line ${jam_lines})
    if(line MATCHES "^[ \t]*obj[ \t]+([A-Za-z0-9_]+)[ \t]*:[ \t]*([A-Za-z0-9_]+)\\.cpp")
      list(APPEND obj_names "${CMAKE_MATCH_1}")
      list(APPEND obj_sources "${spirit_test_dir}/${suite}/${CMAKE_MATCH_2}.cpp")
    endif()
  endforeach()

  foreach(line ${jam_lines})
    set(kind "")
    # `compile-fail` cannot match either branch: both require whitespace after the keyword.
    if(line MATCHES "^[ \t]*run[ \t]+([A-Za-z0-9_]+)\\.cpp(.*)$")
      set(kind run)
    elseif(line MATCHES "^[ \t]*compile[ \t]+([A-Za-z0-9_]+)\\.cpp(.*)$")
      set(kind compile)
    endif()
    if(kind STREQUAL "")
      continue()
    endif()
    set(name "${CMAKE_MATCH_1}")
    set(rest "${CMAKE_MATCH_2}")
    math(EXPR declared "${declared} + 1")
    if("${suite}/${name}" IN_LIST SPIRIT_TESTS_EXCLUDE)
      list(APPEND skipped "${suite}/${name}")
      continue()
    endif()

    # `run rule_separate_tu.cpp rule_separate_tu_grammar ;` -- the second name is an `obj`
    # declared above, and the program does not link without it.
    set(extra "")
    foreach(o ${obj_names})
      if(rest MATCHES "(^|[ \t])${o}([ \t@]|$)")
        list(FIND obj_names "${o}" oi)
        list(GET obj_sources ${oi} extra)
      endif()
    endforeach()

    add_spirit_test(${kind} ${suite} ${name} "${extra}")
    math(EXPR built "${built} + 1")
  endforeach()
endforeach()

# Two lex tests include a header that another test program writes at build time. The Jamfile
# says so with `<dependency>` and `[ location matlib_static.h ]`, and the generator takes its
# output path as argv[1] -- so this ports faithfully rather than being excluded. The output
# goes to the build tree, never back into the vendored Boost checkout: writing there would
# dirty the source of a benchmark whose no-op row is supposed to do nothing.
set(generated_dir "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated_dir}")

function(wire_generated_header gen out_name consumer)
  if(NOT TARGET ${gen} OR NOT TARGET ${consumer})
    return()
  endif()
  set(out "${generated_dir}/${out_name}")
  add_custom_command(
    OUTPUT "${out}"
    COMMAND $<TARGET_FILE:${gen}> "${out}"
    DEPENDS ${gen}
    COMMENT "Generating ${out_name} with ${gen}"
    VERBATIM)
  add_custom_target(${consumer}_generated DEPENDS "${out}")
  add_dependencies(${consumer} ${consumer}_generated)
  target_include_directories(${consumer} PRIVATE "${generated_dir}")
endfunction()

wire_generated_header(spirit_test_lex_regression_matlib_generate
                      matlib_static.h
                      spirit_test_lex_regression_matlib_static)
wire_generated_header(spirit_test_lex_regression_matlib_generate_switch
                      matlib_static_switch.h
                      spirit_test_lex_regression_matlib_switch)

list(LENGTH skipped n_skipped)
message(STATUS "Boost.Spirit tests: ${declared} declared in the Jamfiles, ${built} built, "
               "${n_skipped} excluded")
foreach(s ${skipped})
  message(STATUS "  excluded: ${s}")
endforeach()
