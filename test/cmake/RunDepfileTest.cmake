# Checks what the launcher tells the build system a split object depends on.
#
# Split pieces include rewritten copies of the headers, so that is what the compiler records.
# If the depfile names only those copies, editing the real header rebuilds nothing: the
# copies live in the build directory and change only when the splitter runs, which happens
# only once the build system has already decided the object is stale. The build then links
# an object compiled against the previous version of the header and reports success.
#
# So the assertion is on the depfile itself, which is the contract with the build system,
# rather than on a simulated incremental build.

foreach(required SPLITTER SOURCE HEADER WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

get_filename_component(source_dir "${SOURCE}" DIRECTORY)
set(object "${WORKDIR}/unit.o")
set(depfile "${WORKDIR}/unit.d")

execute_process(
  COMMAND "${SPLITTER}" "${CXX}" -std=c++17 "-I${source_dir}"
          -MD -MF "${depfile}" -MT "${object}"
          -c -o "${object}" "${SOURCE}"
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
if(NOT EXISTS "${depfile}")
  message(FATAL_ERROR "no dependency file was written:\n${launch_log}")
endif()
if(NOT IS_DIRECTORY "${object}.split")
  message(FATAL_ERROR "${SOURCE} was compiled without being split:\n${launch_log}")
endif()

file(READ "${depfile}" deps)

# The header really is split, so its rewritten copy is what the compiler saw. If it is not
# there the test is no longer exercising the case it was written for.
if(NOT deps MATCHES "\\.split/include/")
  message(FATAL_ERROR
    "no rewritten header in the dependency file, so this test is not covering the case it "
    "was written for:\n${deps}")
endif()

foreach(required_path "${HEADER}" "${SOURCE}")
  if(NOT deps MATCHES "${required_path}")
    message(FATAL_ERROR
      "dependency file does not name ${required_path}, so editing it would rebuild "
      "nothing:\n${deps}")
  endif()
endforeach()

message(STATUS "ok: dependency file names the original source and header")
