# Namespace-scope variables that have to stay exactly where they are. TODO/32 and TODO/33.
#
# Two shapes, one program, because both have the same answer: leave the definition in place and
# mark it `inline`, which C++17 allows and which makes the copies every piece gets merge.
#
#   - a variable defined in a *header* that declares no functions. It used to be skipped with
#     "no function definitions", so the header was never rewritten and the preamble included it
#     verbatim -- one definition per piece, and `ld -r` rejects them.
#   - an array whose bound comes from its initialiser. Moving it leaves `extern Row rows[];`,
#     which is a complete-enough declaration for almost everything and not for `sizeof`.
#
# Both used to end in a fallback rather than a wrong program, so the assertion that discriminates
# is that the split ran at all. The mirror check below is what says *why* it now works: had the
# header been rewritten without `inline`, the link would fail the same way it used to.

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
            "${SPLITTER}" "${CXX}" -std=c++17 "-I${WORKDIR}/inc"
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

# The header defines variables and no functions, so it has to be rewritten anyway.
set(mirror_header "${object}.split/include/pkg/hv_constants.hpp")
if(NOT EXISTS "${mirror_header}")
  message(FATAL_ERROR
    "the variable-only header was not rewritten, so its definitions are still being compiled "
    "once per piece and this test is not covering what it was written for:\n${log}")
endif()
file(READ "${mirror_header}" mirror_text)
if(NOT mirror_text MATCHES "inline[ \t]+char const\\*[ \t]+hv_greeting")
  string(REGEX MATCH "[^\n]*hv_greeting[^\n]*" got "${mirror_text}")
  message(FATAL_ERROR
    "the header's variable should be left in place and marked inline, but reads: '${got}'")
endif()

# The array keeps its initialiser, so sizeof still has a bound to read.
file(READ "${object}.split/header_variable_main_preamble.h" preamble_text)
if(NOT preamble_text MATCHES "inline[ \t]+Row[ \t]+rows\\[\\]")
  string(REGEX MATCH "[^\n]*rows\\[\\][^\n]*" got "${preamble_text}")
  message(FATAL_ERROR
    "the deduced-bound array should stay in the preamble marked inline, but reads: '${got}'")
endif()

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
if(NOT out STREQUAL "3 3 5")
  message(FATAL_ERROR "expected '3 3 5', got '${out}'")
endif()

message(STATUS "ok: header variables and deduced-bound arrays stay in place, marked inline")
