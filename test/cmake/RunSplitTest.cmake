# Drives one CLI-mode split test: split a fixture into one file per function, compile and
# link those, then run the result.
#
# A passing run is not enough on its own. When the split output fails to compile the
# splitter falls back to compiling the original source, and the program then runs correctly
# while testing nothing -- so the fallback is treated as a failure here, as is producing no
# split files at all.

foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

set(split_dir "${WORKDIR}/split")
set(program "${WORKDIR}/program")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env CPP_SPLITTER_NO_SERVER=1
          "${SPLITTER}" "${SOURCE}" "${split_dir}"
          --compile -o "${program}" --cxx "${CXX}"
  OUTPUT_VARIABLE split_stdout
  ERROR_VARIABLE split_stderr
  RESULT_VARIABLE split_result)
set(split_log "${split_stdout}${split_stderr}")

if(NOT split_result EQUAL 0)
  message(FATAL_ERROR "cpp-splitter failed (${split_result}) on ${SOURCE}:\n${split_log}")
endif()

if(split_log MATCHES "fallback|falling back")
  message(FATAL_ERROR
    "split output did not compile; cpp-splitter fell back to compiling ${SOURCE} whole:\n${split_log}")
endif()

file(GLOB split_pieces "${split_dir}/*_*.cpp")
if(NOT split_pieces)
  message(FATAL_ERROR "no split files were produced for ${SOURCE}:\n${split_log}")
endif()

if(NOT EXISTS "${program}")
  message(FATAL_ERROR "no program was produced for ${SOURCE}:\n${split_log}")
endif()

execute_process(
  COMMAND "${program}"
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
  RESULT_VARIABLE run_result)

if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "program built from the split pieces of ${SOURCE} failed (${run_result}):\n${run_stdout}${run_stderr}")
endif()

list(LENGTH split_pieces piece_count)
message(STATUS "ok: ${SOURCE} -> ${piece_count} split file(s), program exited 0")
