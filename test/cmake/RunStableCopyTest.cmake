# Three units through the launcher: a.cpp calls alpha(), b.cpp calls beta(), both from ops.h.
# After an edit to alpha()'s body, b.cpp's copy of ops.h must be byte-identical, its object
# untouched, and the program must print alpha's new result. TODO/48.
foreach(required SPLITTER SOURCE_DIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/src")
foreach(f ops.h a.cpp b.cpp main.cpp)
  configure_file("${SOURCE_DIR}/${f}" "${WORKDIR}/src/${f}" COPYONLY)
endforeach()

function(compile unit label)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1
            "${SPLITTER}" "${CXX}" -std=c++17 -MD -MF "${WORKDIR}/${unit}.d" -MT "${WORKDIR}/${unit}.o"
            -c -o "${WORKDIR}/${unit}.o" "${WORKDIR}/src/${unit}.cpp"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(log "${o}${e}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: launcher failed on ${unit}.cpp (${rc}):\n${log}")
  endif()
  if(log MATCHES "fallback|falling back|not splitting")
    message(FATAL_ERROR "${label}: ${unit}.cpp was not split:\n${log}")
  endif()
  set(${unit}_log "${log}" PARENT_SCOPE)
endfunction()

function(link_and_check expect label)
  execute_process(COMMAND "${CXX}" -o "${WORKDIR}/program" "${WORKDIR}/a.o" "${WORKDIR}/b.o" "${WORKDIR}/main.o"
                  RESULT_VARIABLE lrc OUTPUT_VARIABLE lo ERROR_VARIABLE le)
  if(NOT lrc EQUAL 0)
    message(FATAL_ERROR "${label}: link failed:\n${lo}${le}")
  endif()
  execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
  string(STRIP "${out}" out)
  if(NOT prc EQUAL 0 OR NOT out STREQUAL "${expect}")
    message(FATAL_ERROR "${label}: program wrong: rc=${prc} out='${out}' expected '${expect}'")
  endif()
endfunction()

compile(a first)
compile(b first)
compile(main first)
link_and_check("60 3" first)

set(b_copy "${WORKDIR}/b.o.split/include/ops.h")
if(NOT EXISTS "${b_copy}")
  message(FATAL_ERROR "no copy of ops.h for b.cpp at ${b_copy}")
endif()
file(READ "${b_copy}" b_copy_before)
if(b_copy_before MATCHES "return v \\* 10")
  message(FATAL_ERROR "b.cpp's copy of ops.h defines alpha(), which b.cpp does not emit:\n${b_copy_before}")
endif()
if(NOT b_copy_before MATCHES "int alpha\\(int v\\);")
  message(FATAL_ERROR "b.cpp's copy of ops.h does not declare alpha():\n${b_copy_before}")
endif()
# The pieces' objects, not b.o itself: the launcher touches an object it leaves as it is,
# so that the build system does not rebuild it every time.
file(GLOB b_pieces "${WORKDIR}/b.o.split/*.o" "${WORKDIR}/b.o.split/include/*.o")
set(b_obj_before "")
foreach(o ${b_pieces})
  file(TIMESTAMP "${o}" t "%s")
  string(APPEND b_obj_before "${o}=${t};")
endforeach()

# Edit alpha()'s body.
file(READ "${WORKDIR}/src/ops.h" ops)
string(REPLACE "return v * 10;" "return v * 100;" ops "${ops}")
file(WRITE "${WORKDIR}/src/ops.h" "${ops}")

compile(a edit)
compile(b edit)
compile(main edit)
link_and_check("600 3" edit)

file(READ "${b_copy}" b_copy_after)
if(NOT b_copy_after STREQUAL b_copy_before)
  message(FATAL_ERROR "b.cpp's copy of ops.h changed under an edit to a body b.cpp does not emit")
endif()
set(b_obj_after "")
foreach(o ${b_pieces})
  file(TIMESTAMP "${o}" t "%s")
  string(APPEND b_obj_after "${o}=${t};")
endforeach()
if(NOT b_obj_after STREQUAL b_obj_before OR b_log MATCHES "compile \\(seq\\)|compiling [0-9]+ split")
  message(FATAL_ERROR "b.cpp's pieces were recompiled for an edit to a body it does not emit:\n${b_log}")
endif()
if(NOT b_log MATCHES "re-sliced nothing")
  message(FATAL_ERROR "b.cpp did not take the declared-only path:\n${b_log}")
endif()
message(STATUS "ok: b.cpp's copy and object are untouched by the edit; the program prints the new result")
