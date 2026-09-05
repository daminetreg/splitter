# Splits a unit, edits a header it includes, and splits again.
#
# The second split is the whole test. The prefix PCH is named after a hash of the prefix file
# -- the include directives at the top of the source -- which does not change when one of the
# headers it names is edited. A PCH keyed on that alone is silently stale, and libclang does
# not degrade gracefully when handed one: the parse fails with CXError_ASTReadError and the
# unit falls back to compiling whole. Nothing says so unless someone reads the verbose log,
# so this test asserts on the log as well as on the program's answer.

foreach(required SPLITTER SOURCE HEADER WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

get_filename_component(header_name "${HEADER}" NAME)
get_filename_component(source_name "${SOURCE}" NAME)
configure_file("${HEADER}" "${WORKDIR}/${header_name}" COPYONLY)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)

# Both splits go to the same object path on purpose: that is what makes the second one reuse
# the first one's split directory, and with it the prefix PCH under test. Splitting to a
# fresh path would find no PCH to be stale and would pass whatever the splitter does.
function(split_and_run expected label)
  set(object "${WORKDIR}/unit.o")
  execute_process(
    COMMAND   "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
              "${SPLITTER}" "${CXX}" "-I${WORKDIR}"
              -c -o "${object}" "${WORKDIR}/${source_name}"
    OUTPUT_VARIABLE launch_stdout
    ERROR_VARIABLE launch_stderr
    RESULT_VARIABLE launch_result)
  set(launch_log "${launch_stdout}${launch_stderr}")

  if(NOT launch_result EQUAL 0)
    message(FATAL_ERROR "${label}: launcher failed (${launch_result}):\n${launch_log}")
  endif()
  if(launch_log MATCHES "fallback|falling back")
    message(FATAL_ERROR "${label}: cpp-splitter fell back instead of splitting:\n${launch_log}")
  endif()
  if(NOT IS_DIRECTORY "${object}.split")
    message(FATAL_ERROR "${label}: the source was compiled without being split:\n${launch_log}")
  endif()

  set(program "${WORKDIR}/${label}")
  execute_process(
    COMMAND "${CXX}" -o "${program}" "${object}"
    RESULT_VARIABLE link_result OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT link_result EQUAL 0)
    message(FATAL_ERROR "${label}: linking failed (${link_result}):\n${o}${e}")
  endif()

  execute_process(COMMAND "${program}" OUTPUT_VARIABLE run_stdout RESULT_VARIABLE run_result)
  string(STRIP "${run_stdout}" run_stdout)
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "${label}: the program failed (${run_result}), printed '${run_stdout}'")
  endif()
  if(NOT run_stdout STREQUAL expected)
    message(FATAL_ERROR "${label}: expected '${expected}', got '${run_stdout}'")
  endif()
endfunction()

split_and_run("7" first)

# Change the body of the inline function the header defines. The prefix PCH's own name does
# not change, because the source's include directives did not.
file(READ "${WORKDIR}/${header_name}" header_text)
string(REPLACE "return 3 + 4;" "return 3 + 4 + 100;" header_text "${header_text}")
file(WRITE "${WORKDIR}/${header_name}" "${header_text}")

split_and_run("107" second)

message(STATUS "ok: the split follows an edit to a header behind the prefix PCH")
