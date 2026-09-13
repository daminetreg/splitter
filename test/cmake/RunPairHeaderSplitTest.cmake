# A header included twice with no include guard under two macro states, whose second
# expansion defines an external function. One mirrored copy cannot serve both inclusions, so
# each is split on its own: the first into the usual copy, the second into
# include/_inclusion/2/<header>, and the unit's preamble is rewritten to name that copy on its
# second #include line. The unit must split, the second expansion's function must have a
# piece under the second copy, and the program must be right.
foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(split_dir "${WORKDIR}/split")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
          "${SPLITTER}" "${SOURCE}" "${split_dir}" --compile -o "${WORKDIR}/program" --cxx "${CXX}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cpp-splitter failed (${rc}):\n${log}")
endif()
if(log MATCHES "fallback|falling back|not splitting|declined")
  message(FATAL_ERROR "the unit was not split:\n${log}")
endif()
file(GLOB second_pieces "${split_dir}/include/_inclusion/2/pair_header.hpp_*_kernel.cpp")
if(NOT second_pieces)
  message(FATAL_ERROR "no piece for the second inclusion's kernel() under include/_inclusion/2:\n${log}")
endif()
if(NOT EXISTS "${split_dir}/include/_inclusion/2/pair_header.hpp")
  message(FATAL_ERROR "no second copy of the header was written")
endif()
# The macro-produced definitions are compiled once, in place, behind the preamble cut before
# the second inclusion: the definitions piece includes that context and the copy's variant.
if(NOT EXISTS "${split_dir}/include/_inclusion/2/pair_header.hpp.definitions.h")
  message(FATAL_ERROR "no definitions variant of the second copy was written")
endif()
file(READ "${split_dir}/include/_inclusion/2/pair_header.hpp_0_definitions.cpp" defpiece)
if(NOT defpiece MATCHES "pair_header_main_preamble\\.pair_header\\.hpp\\.2\\.before\\.h")
  message(FATAL_ERROR "the definitions piece does not include the context cut before the second inclusion:\n${defpiece}")
endif()
file(READ "${split_dir}/pair_header_main_preamble.h" preamble)
string(REGEX MATCHALL "#include \"[^\"]*pair_header.hpp\"" includes "${preamble}")
list(LENGTH includes n)
if(NOT n EQUAL 2)
  message(FATAL_ERROR "expected two #include lines for the header in the preamble, found ${n}:\n${preamble}")
endif()
list(GET includes 1 second)
if(NOT second MATCHES "_inclusion/2/pair_header.hpp")
  message(FATAL_ERROR "the preamble's second #include does not name the second copy: ${second}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "31 15 6")
  message(FATAL_ERROR "program wrong: rc=${prc} out='${out}'")
endif()

# A second run over the same split directory reuses the second copy's split through its
# manifest under _inclusion/2/, and the program is still right.
file(REMOVE "${WORKDIR}/program")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
          "${SPLITTER}" "${SOURCE}" "${split_dir}" --compile -o "${WORKDIR}/program" --cxx "${CXX}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "second run failed (${rc}):\n${log}")
endif()
if(log MATCHES "fallback|falling back|not splitting|declined")
  message(FATAL_ERROR "the unit was not split on the second run:\n${log}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "31 15 6")
  message(FATAL_ERROR "program wrong after the second run: rc=${prc} out='${out}'")
endif()
message(STATUS "ok: both inclusions split, the second into its own copy, and the program is right")
