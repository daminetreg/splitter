# Drives one compiler-launcher test across several translation units: each source is
# compiled through cpp-splitter acting as CMAKE_CXX_COMPILER_LAUNCHER, then the resulting
# objects are linked together and the program is run.
#
# This is the shape that matters for a header shared between translation units: each one
# splits the header into its own directory, so every object ends up carrying a copy of the
# header's functions and the final link has to merge rather than reject them.

foreach(required SPLITTER SOURCES INCLUDE_DIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

set(objects)
foreach(source IN LISTS SOURCES)
  get_filename_component(stem "${source}" NAME_WE)
  set(object "${WORKDIR}/${stem}.o")

  execute_process(
    COMMAND   "${SPLITTER}" "${CXX}" -std=c++17 "-I${INCLUDE_DIR}"
            -c -o "${object}" "${source}"
    OUTPUT_VARIABLE launch_stdout
    ERROR_VARIABLE launch_stderr
    RESULT_VARIABLE launch_result)
  set(launch_log "${launch_stdout}${launch_stderr}")

  if(NOT launch_result EQUAL 0)
    message(FATAL_ERROR "launcher failed (${launch_result}) on ${source}:\n${launch_log}")
  endif()

  if(launch_log MATCHES "fallback|falling back")
    message(FATAL_ERROR
      "split output did not compile; cpp-splitter fell back on ${source}:\n${launch_log}")
  endif()

  if(NOT EXISTS "${object}")
    message(FATAL_ERROR "no object was produced for ${source}:\n${launch_log}")
  endif()

  if(NOT IS_DIRECTORY "${object}.split")
    message(FATAL_ERROR "${source} was compiled without being split:\n${launch_log}")
  endif()

  list(APPEND objects "${object}")
endforeach()

set(program "${WORKDIR}/program")
execute_process(
  COMMAND "${CXX}" -o "${program}" ${objects}
  OUTPUT_VARIABLE link_stdout
  ERROR_VARIABLE link_stderr
  RESULT_VARIABLE link_result)

if(NOT link_result EQUAL 0)
  message(FATAL_ERROR "linking the split objects failed (${link_result}):\n${link_stdout}${link_stderr}")
endif()

execute_process(
  COMMAND "${program}"
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
  RESULT_VARIABLE run_result)

if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "program failed (${run_result}):\n${run_stdout}${run_stderr}")
endif()

message(STATUS "ok: ${SOURCES} linked from split objects, program exited 0")
