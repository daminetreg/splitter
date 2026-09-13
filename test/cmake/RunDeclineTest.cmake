# A unit the splitter must decline: split nothing, say why, compile whole, and be right.
# EXPECT_MESSAGE is a regular expression the splitter's output must match; EXPECT_OUTPUT is
# what the program must print. Two shapes use it today -- a pair header that defines an
# external function, and a static variable of a type no other translation unit can name --
# and in both a split that went ahead was worse than a fallback: the first failed at the
# relocatable link after every piece was compiled, the second built a program that was wrong.
foreach(required SPLITTER SOURCE WORKDIR CXX EXPECT_MESSAGE EXPECT_OUTPUT)
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
if(NOT log MATCHES "${EXPECT_MESSAGE}")
  message(FATAL_ERROR "expected the splitter to decline the unit and say why (${EXPECT_MESSAGE}):\n${log}")
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
if(NOT prc EQUAL 0 OR NOT out STREQUAL "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "the program compiled whole is wrong: rc=${prc} out='${out}' expected '${EXPECT_OUTPUT}'")
endif()
message(STATUS "ok: declined with the reason, nothing written, program right")
