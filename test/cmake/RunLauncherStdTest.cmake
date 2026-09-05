# Drives the launcher with no -std on the command line, and checks that the split object
# behaves like the plain one.
#
# The point of the test is what the *absence* of -std does. When the command line names no
# standard, the compiler driver and libclang each fall back to their own default, and the
# two need not agree -- clang 13's driver defaults to gnu++14 while libclang parses at
# C++17. So this driver must not add one, and neither may the fixture's CMake target.

foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

# What the compiler itself says, with the same empty command line the splitter will see.
set(plain "${WORKDIR}/plain")
execute_process(
  COMMAND "${CXX}" -o "${plain}" "${SOURCE}"
  RESULT_VARIABLE plain_result
  OUTPUT_VARIABLE plain_out
  ERROR_VARIABLE plain_err)
if(NOT plain_result EQUAL 0)
  message(FATAL_ERROR "the fixture does not compile unsplit:\n${plain_out}${plain_err}")
endif()
execute_process(COMMAND "${plain}" OUTPUT_VARIABLE plain_stdout RESULT_VARIABLE plain_run)
if(NOT plain_run EQUAL 0)
  message(FATAL_ERROR "the fixture does not run unsplit (${plain_run})")
endif()
string(STRIP "${plain_stdout}" plain_stdout)

set(object "${WORKDIR}/split.o")
execute_process(
  COMMAND   "${SPLITTER}" "${CXX}" -c -o "${object}" "${SOURCE}"
  OUTPUT_VARIABLE launch_stdout
  ERROR_VARIABLE launch_stderr
  RESULT_VARIABLE launch_result)
set(launch_log "${launch_stdout}${launch_stderr}")

if(NOT launch_result EQUAL 0)
  message(FATAL_ERROR "launcher failed (${launch_result}):\n${launch_log}")
endif()
if(launch_log MATCHES "fallback|falling back")
  message(FATAL_ERROR "split output did not compile; cpp-splitter fell back:\n${launch_log}")
endif()
if(NOT IS_DIRECTORY "${object}.split")
  message(FATAL_ERROR "the source was compiled without being split:\n${launch_log}")
endif()

set(program "${WORKDIR}/split")
execute_process(
  COMMAND "${CXX}" -o "${program}" "${object}"
  RESULT_VARIABLE link_result
  OUTPUT_VARIABLE link_out
  ERROR_VARIABLE link_err)
if(NOT link_result EQUAL 0)
  message(FATAL_ERROR "linking the split object failed (${link_result}):\n${link_out}${link_err}")
endif()

execute_process(COMMAND "${program}" OUTPUT_VARIABLE split_stdout RESULT_VARIABLE split_run)
if(NOT split_run EQUAL 0)
  message(FATAL_ERROR "the split program failed (${split_run})")
endif()
string(STRIP "${split_stdout}" split_stdout)

# The real check: the splitter parsed the same program the compiler compiles. If libclang
# took a different branch of the #if, the two answers differ.
if(NOT split_stdout STREQUAL plain_stdout)
  message(FATAL_ERROR
    "split and plain disagree: plain said '${plain_stdout}', split said '${split_stdout}'.\n"
    "libclang parsed a different language standard than the driver compiles at.")
endif()

message(STATUS "ok: split and plain both said '${plain_stdout}'")
