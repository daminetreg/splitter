# Two translation units (UNITS, a ;-list of stems) through the launcher, linked together and run. What one unit
# defines and only the other reads has to survive the split: a variable left in the
# preamble as `inline` is emitted only where something uses it, and settings.cpp's pieces
# use neither of its two (TODO/47). Both units must split; the program must print EXPECT.
foreach(required SPLITTER SOURCE_DIR UNITS WORKDIR CXX EXPECT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(objects)
foreach(unit ${UNITS})
  set(object "${WORKDIR}/${unit}.o")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
            "${SPLITTER}" "${CXX}" -std=c++17 -c -o "${object}" "${SOURCE_DIR}/${unit}.cpp"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(log "${o}${e}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "launcher failed on ${unit}.cpp (${rc}):\n${log}")
  endif()
  if(log MATCHES "fallback|falling back|not splitting")
    message(FATAL_ERROR "${unit}.cpp was not split:\n${log}")
  endif()
  list(APPEND objects "${object}")
endforeach()
execute_process(COMMAND "${CXX}" -o "${WORKDIR}/program" ${objects}
                RESULT_VARIABLE lrc OUTPUT_VARIABLE lo ERROR_VARIABLE le)
if(NOT lrc EQUAL 0)
  message(FATAL_ERROR "link failed:\n${lo}${le}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "${EXPECT}")
  message(FATAL_ERROR "program wrong: rc=${prc} out='${out}' expected '${EXPECT}'")
endif()
message(STATUS "ok: both units split and the program prints ${EXPECT}")
