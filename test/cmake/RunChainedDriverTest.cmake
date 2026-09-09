# cpp-splitter invoked behind another compiler launcher. TODO/34.
#
# cmake-re composes CMAKE_CXX_COMPILER_LAUNCHER as "<cpp-splitter>;tipi-compiler-driver", so
# the splitter is run as `cpp-splitter tipi-compiler-driver clang++ <flags>` and argv[1] is a
# launcher rather than a compiler. It used to probe argv[1] for the system include paths and
# the default language standard; probing a launcher gets nothing back, and the parse then sees
# a translation unit the compiler would not recognise.
#
# The damage surfaced a long way from the cause: `inline` inserted into the middle of an alias
# template in boost/mp11/algorithm.hpp, five Boost.Spirit units falling back, and a defect
# report about alias templates that had nothing to do with alias templates.
#
# The stub below stands in for the real driver so this test needs nothing from the toolchain:
# `exec "$@"` runs whatever it is handed, which is exactly what the driver does with a compile
# command -- and exactly what it cannot do with `-x c++ -E -dM /dev/null`, so the probe fails
# here the same way it fails there.

foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/bin")
get_filename_component(source_name "${SOURCE}" NAME)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)

set(stub "${WORKDIR}/bin/tipi-compiler-driver")
file(WRITE "${stub}" "#!/bin/sh\nexec \"$@\"\n")
execute_process(COMMAND chmod +x "${stub}")

set(object "${WORKDIR}/unit.o")
execute_process(
  COMMAND   "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1 "PATH=${WORKDIR}/bin:$ENV{PATH}"
            "${SPLITTER}" tipi-compiler-driver "${CXX}" -std=c++17
            -c -o "${object}" "${WORKDIR}/${source_name}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")
# Kept on disk unconditionally: a failure here is diagnosed by reading what the launcher said,
# and a message() truncates.
file(WRITE "${WORKDIR}/launcher.log" "${log}")

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "launcher failed (${rc}):\n${log}")
endif()
if(log MATCHES "fallback|falling back")
  message(FATAL_ERROR
    "cpp-splitter fell back while chained behind a launcher. This is TODO/34: the system "
    "include paths were probed from the wrong program.\n${log}")
endif()
if(NOT IS_DIRECTORY "${object}.split")
  message(FATAL_ERROR "the source was compiled without being split:\n${log}")
endif()

# The assertion that actually pins TODO/34.
#
# "no fallback" is not enough on its own: this unit includes only <string>, and clang finds
# that through its own resource directory whether or not the splitter asked the compiler where
# its system headers are. The bug is silent here and loud on Boost.Mp11, which is precisely how
# it survived long enough to be misfiled as an alias-template defect.
#
# What is never silent is the probe itself. `parse standard: ... (probed)` is printed only when
# the splitter got an answer back, and a launcher cannot answer `-x c++ -E -dM /dev/null`. Its
# absence is the defect, one step from the cause instead of several from the symptom.
if(NOT log MATCHES "parse standard: [^\n]*\\(probed\\)")
  message(FATAL_ERROR
    "the splitter never probed a real compiler for the parse standard, so it probed the "
    "launcher in front of it and would have parsed without the compiler's system include "
    "paths. This is TODO/34.\nSee ${WORKDIR}/launcher.log")
endif()

# The split has to have produced real pieces, or "no fallback" would be satisfied by a unit
# that had nothing to split in the first place.
file(GLOB pieces "${object}.split/*.cpp")
list(LENGTH pieces n_pieces)
if(n_pieces LESS 2)
  message(FATAL_ERROR
    "expected the unit to split into pieces, found ${n_pieces}; this test would pass without "
    "exercising anything")
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
if(NOT out STREQUAL "chained 7")
  message(FATAL_ERROR "expected 'chained 7', got '${out}'")
endif()

message(STATUS "ok: splitting behind another launcher probes the real compiler")
