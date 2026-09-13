# A header included twice with no include guard is one half of a pair and is left unsplit.
# When it defines a function with external linkage, the unit cannot be split either: left as
# it is, the header reaches every piece through the preamble. The splitter must say so, do no
# piece work, and compile the unit whole -- and the program must be right.
foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
          "${SPLITTER}" "${SOURCE}" "${WORKDIR}/split" --compile -o "${WORKDIR}/program" --cxx "${CXX}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
set(log "${o}${e}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cpp-splitter failed (${rc}):\n${log}")
endif()
if(NOT log MATCHES "included more than once with no include guard and defines a function with external linkage")
  message(FATAL_ERROR "expected the splitter to decline the unit and say why:\n${log}")
endif()
if(log MATCHES "relocatable link failed|multiple definition")
  message(FATAL_ERROR "the decision came after the pieces were built, not before:\n${log}")
endif()
file(GLOB pieces "${WORKDIR}/split/*_*.cpp")
if(pieces)
  message(FATAL_ERROR "pieces were written for a unit that was declined: ${pieces}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "31")
  message(FATAL_ERROR "the program compiled whole is wrong: rc=${prc} out='${out}'")
endif()
message(STATUS "ok: a twice-included header defining an external function declines the unit before any piece is written")
