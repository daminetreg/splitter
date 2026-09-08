# A rewritten header includes a sibling by quoted, directory-relative path. TODO/31.
#
# `#include "x.hpp"` is resolved relative to the directory of the file doing the including.
# Once a header is rewritten into the mirror under <split_dir>/include, that is the directory
# searched -- and the mirror holds a copy only for headers that were themselves split. A
# sibling that produced no copy is then unreachable, because the include root is above the
# header's own directory and the unqualified name matches nothing on the -I list.
#
# The layout matters and is not incidental. The headers sit in a `pkg/` subdirectory with only
# its parent on the include path, which is the shape Boost.Spirit's lexer headers have. Put
# them flat in the same directory as the source and the sibling is found through -I whatever
# the splitter does, and the test passes without testing anything.
#
# Two assertions, one for each half of the rule:
#   - quoted_sibling.hpp has no copy, so quoted_split.hpp's directive must be rewritten to the
#     original's absolute path;
#   - quoted_partner.hpp does have one, so quoted_pair.hpp's directive must be left alone. The
#     mirror is parallel to the original tree, so the relative path still names the right file,
#     and pointing it at the original would read a header whose definitions were split out.

foreach(required SPLITTER SOURCE HEADERDIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/inc/pkg")
get_filename_component(source_name "${SOURCE}" NAME)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)
file(GLOB pkg_headers "${HEADERDIR}/*.hpp")
foreach(h ${pkg_headers})
  get_filename_component(hn "${h}" NAME)
  configure_file("${h}" "${WORKDIR}/inc/pkg/${hn}" COPYONLY)
endforeach()

set(object "${WORKDIR}/unit.o")
execute_process(
  COMMAND   "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
            "${SPLITTER}" "${CXX}" "-I${WORKDIR}/inc"
            -MD -MF "${WORKDIR}/unit.d" -MT "${object}"
            -c -o "${object}" "${WORKDIR}/${source_name}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "launcher failed (${rc}):\n${log}")
endif()
if(log MATCHES "fallback|falling back")
  message(FATAL_ERROR "cpp-splitter fell back instead of splitting:\n${log}")
endif()
if(NOT IS_DIRECTORY "${object}.split")
  message(FATAL_ERROR "the source was compiled without being split:\n${log}")
endif()

set(mirror "${object}.split/include/pkg")

# The header under test must actually have been rewritten, or neither assertion below means
# anything.
if(NOT EXISTS "${mirror}/quoted_split.hpp")
  message(FATAL_ERROR
    "quoted_split.hpp was not rewritten into the mirror, so this test is not covering the "
    "case it was written for. Mirror contents follow:\n${mirror}")
endif()

# Half one: the sibling has no copy, so the directive must now name the original by absolute
# path.
if(EXISTS "${mirror}/quoted_sibling.hpp")
  message(FATAL_ERROR
    "quoted_sibling.hpp has a copy in the mirror; it was chosen because it should not, and "
    "with one present this test no longer exercises the rewrite")
endif()
file(READ "${mirror}/quoted_split.hpp" split_text)
if(NOT split_text MATCHES "#include \"${WORKDIR}/inc/pkg/quoted_sibling.hpp\"")
  string(REGEX MATCH "#include \"[^\"]*quoted_sibling[^\"]*\"" got "${split_text}")
  message(FATAL_ERROR
    "quoted_split.hpp's include of its unmirrored sibling was not rewritten to the original: "
    "found '${got}'")
endif()

# Half two: the partner does have a copy, so that directive must be untouched.
if(NOT EXISTS "${mirror}/quoted_partner.hpp")
  message(FATAL_ERROR
    "quoted_partner.hpp has no copy in the mirror, so the leave-it-alone half of the rule is "
    "not being exercised")
endif()
file(READ "${mirror}/quoted_pair.hpp" pair_text)
if(NOT pair_text MATCHES "#include \"quoted_partner\\.hpp\"")
  string(REGEX MATCH "#include \"[^\"]*quoted_partner[^\"]*\"" got "${pair_text}")
  message(FATAL_ERROR
    "quoted_pair.hpp's include of its mirrored partner should have been left alone, but reads "
    "'${got}'. Pointing it at the original reads a header whose definitions were split out.")
endif()

# And the program the split object makes has to work.
set(program "${WORKDIR}/prog")
execute_process(COMMAND "${CXX}" -o "${program}" "${object}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "linking failed (${rc}):\n${o}${e}")
endif()
execute_process(COMMAND "${program}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
string(STRIP "${out}" out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the program failed (${rc}), printed '${out}'")
endif()
if(NOT out STREQUAL "42 8")
  message(FATAL_ERROR "expected '42 8', got '${out}'")
endif()

message(STATUS "ok: quoted includes resolve in the mirror, and mirrored siblings are left alone")
