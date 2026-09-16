# CPP_SPLITTER_REMOTE_JOBS caps how many piece compiles one launcher runs at once behind the
# tipi compiler driver. TODO/50.
#
# The stub driver counts itself in: one directory per running compile under live/, the count
# appended to a file at each start, a pause so that the compiles overlap, then the compiler.
# Without the cap the launcher starts every piece at once behind the driver; with
# CPP_SPLITTER_REMOTE_JOBS=2 no count may exceed 2. The pieces still link and run.
foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/bin" "${WORKDIR}/live")
get_filename_component(source_name "${SOURCE}" NAME)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)

set(stub "${WORKDIR}/bin/tipi-compiler-driver")
file(WRITE "${stub}" "#!/bin/sh
mkdir \"${WORKDIR}/live/$$\"
ls \"${WORKDIR}/live\" | wc -l >> \"${WORKDIR}/counts\"
sleep 0.3
\"$@\"
rc=$?
rmdir \"${WORKDIR}/live/$$\"
exit $rc
")
execute_process(COMMAND chmod +x "${stub}")

function(run_capped cap label)
  file(REMOVE "${WORKDIR}/counts")
  file(REMOVE_RECURSE "${WORKDIR}/unit.o.split")
  execute_process(
    COMMAND   "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1 "PATH=${WORKDIR}/bin:$ENV{PATH}"
              "CPP_SPLITTER_REMOTE_JOBS=${cap}"
              "${SPLITTER}" tipi-compiler-driver "${CXX}" -std=c++17
              -c -o "${WORKDIR}/unit.o" "${WORKDIR}/${source_name}"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(log "${o}${e}")
  file(WRITE "${WORKDIR}/launcher-${label}.log" "${log}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: launcher failed (${rc}):\n${log}")
  endif()
  if(log MATCHES "fallback|falling back")
    message(FATAL_ERROR "${label}: cpp-splitter fell back:\n${log}")
  endif()
  file(STRINGS "${WORKDIR}/counts" counts)
  set(max 0)
  foreach(c ${counts})
    if(c GREATER max)
      set(max ${c})
    endif()
  endforeach()
  set(${label}_max ${max} PARENT_SCOPE)
  execute_process(COMMAND "${CXX}" -o "${WORKDIR}/program" "${WORKDIR}/unit.o"
                  RESULT_VARIABLE lrc OUTPUT_VARIABLE lo ERROR_VARIABLE le)
  if(NOT lrc EQUAL 0)
    message(FATAL_ERROR "${label}: link failed:\n${lo}${le}")
  endif()
  execute_process(COMMAND "${WORKDIR}/program" RESULT_VARIABLE prc OUTPUT_VARIABLE pout)
  if(NOT prc EQUAL 0)
    message(FATAL_ERROR "${label}: program failed (${prc}):\n${pout}")
  endif()
endfunction()

run_capped(0 uncapped)
if(uncapped_max LESS 3)
  message(FATAL_ERROR "without a cap the pieces did not overlap (at most ${uncapped_max} at once); the test would pass without exercising anything")
endif()
run_capped(2 capped)
if(capped_max GREATER 2)
  message(FATAL_ERROR "CPP_SPLITTER_REMOTE_JOBS=2 let ${capped_max} piece compiles run at once")
endif()
message(STATUS "ok: ${uncapped_max} at once without a cap, ${capped_max} with CPP_SPLITTER_REMOTE_JOBS=2")
