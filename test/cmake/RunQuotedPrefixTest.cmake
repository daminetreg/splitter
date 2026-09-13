# The launcher on a unit whose include block has a quoted include of a sibling header, with
# nothing putting that directory on -I. The libclang prefix PCH must be built from the
# original's point of view -- or not at all: a PCH built around a fatal error is worse than
# none, and the parse that loads it reads a program that is neither the source nor anything
# else. Asserted on the splitter's own report, then on the program.
foreach(required SPLITTER SOURCE WORKDIR CXX API_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(object "${WORKDIR}/unit.o")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
          "${SPLITTER}" "${CXX}" "-I${API_DIR}" -MD -MF "${WORKDIR}/unit.d" -MT "${object}"
          -c -o "${object}" "${SOURCE}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "launcher failed (${rc}):\n${log}")
endif()
if(log MATCHES "fallback|falling back")
  message(FATAL_ERROR "the unit fell back:\n${log}")
endif()
if(log MATCHES "libclang PCH diagnostic|error\\(s\\) while building it")
  message(FATAL_ERROR "the prefix PCH was built with errors:\n${log}")
endif()
if(log MATCHES "Parse error")
  message(FATAL_ERROR "the parse reported errors on a unit the compiler accepts:\n${log}")
endif()
if(NOT log MATCHES "libclang PCH built")
  message(FATAL_ERROR "no prefix PCH was built at all; the case is not exercised:\n${log}")
endif()
execute_process(COMMAND "${CXX}" -o "${WORKDIR}/program" "${object}" RESULT_VARIABLE lrc OUTPUT_VARIABLE lo ERROR_VARIABLE le)
if(NOT lrc EQUAL 0)
  message(FATAL_ERROR "link failed:\n${lo}${le}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "21 9 30")
  message(FATAL_ERROR "program wrong: rc=${prc} out='${out}'")
endif()
message(STATUS "ok: the prefix PCH resolves quoted includes as the unit does, and the parse is clean")
