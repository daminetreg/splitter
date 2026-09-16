# The store shares what is weak and refuses what is strong, on every platform. Two units
# include shared.h: its inline shared_fn() gets one object in the store, linked by both.
# One unit includes once.h, an implementation include with a non-inline strong_once() beside
# the inline once_fn(): the shared piece for once_fn() emits strong_once() as a strong
# symbol, so the store must refuse that object and the unit compile its own piece. On macOS, nm printed every definition as T and the store refused
# everything -- shared_fn() included -- so the two assertions together are the fix's
# proof and its guard against over-correcting into sharing everything. TODO/54.
foreach(required SPLITTER SOURCE_DIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/src" "${WORKDIR}/store")
foreach(f shared.h once.h a.cpp b.cpp main.cpp)
  configure_file("${SOURCE_DIR}/${f}" "${WORKDIR}/src/${f}" COPYONLY)
endforeach()

function(compile unit)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1 "CPP_SPLITTER_STORE=${WORKDIR}/store"
            "${SPLITTER}" "${CXX}" -std=c++17 -MD -MF "${WORKDIR}/${unit}.d" -MT "${WORKDIR}/${unit}.o"
            -c -o "${WORKDIR}/${unit}.o" "${WORKDIR}/src/${unit}.cpp"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(log "${o}${e}")
  file(WRITE "${WORKDIR}/${unit}.log" "${log}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "launcher failed on ${unit}.cpp (${rc}):\n${log}")
  endif()
  if(log MATCHES "fallback|falling back|not splitting")
    message(FATAL_ERROR "${unit}.cpp was not split:\n${log}")
  endif()
  set(${unit}_log "${log}" PARENT_SCOPE)
endfunction()

function(store_key twin out_var)
  file(GLOB twins "${twin}")
  list(LENGTH twins n)
  if(NOT n EQUAL 1)
    message(FATAL_ERROR "expected one piece at ${twin}, found ${n}")
  endif()
  file(STRINGS "${twins}" store_line REGEX "^// Store: ")
  if(NOT store_line)
    message(FATAL_ERROR "${twins} is not a shared piece")
  endif()
  string(REGEX REPLACE "^// Store: " "" key "${store_line}")
  set(${out_var} "${key}" PARENT_SCOPE)
endfunction()

compile(a)
compile(b)
compile(main)
execute_process(COMMAND "${CXX}" -o "${WORKDIR}/program" "${WORKDIR}/a.o" "${WORKDIR}/b.o" "${WORKDIR}/main.o"
                RESULT_VARIABLE lrc OUTPUT_VARIABLE lo ERROR_VARIABLE le)
if(NOT lrc EQUAL 0)
  message(FATAL_ERROR "link failed:\n${lo}${le}")
endif()
execute_process(COMMAND "${WORKDIR}/program" OUTPUT_VARIABLE out RESULT_VARIABLE prc)
string(STRIP "${out}" out)
if(NOT prc EQUAL 0 OR NOT out STREQUAL "117 15")
  message(FATAL_ERROR "program wrong: rc=${prc} out='${out}' expected '117 15'")
endif()

# shared_fn(): weak, so shared -- one object in the store, b.cpp links it without compiling.
store_key("${WORKDIR}/a.o.split/include/shared.h_*_shared_fn.cpp" a_key)
store_key("${WORKDIR}/b.o.split/include/shared.h_*_shared_fn.cpp" b_key)
if(NOT a_key STREQUAL b_key)
  message(FATAL_ERROR "the two units' pieces for shared_fn() have different store keys")
endif()
file(GLOB shared_objs "${WORKDIR}/store/${a_key}-*.o")
list(LENGTH shared_objs n_shared)
if(NOT n_shared EQUAL 1)
  message(FATAL_ERROR "the store holds ${n_shared} object(s) for shared_fn(), expected 1: the weak definition was read as strong")
endif()
if(a_log MATCHES "may exist once only[^\n]*shared\\.h")
  message(FATAL_ERROR "shared_fn()'s shared piece was refused as strong:\n${a_log}")
endif()
if(NOT b_log MATCHES "shared piece \\.o: [^\n]*${b_key}")
  message(FATAL_ERROR "b.cpp did not link the shared piece for shared_fn():\n${b_log}")
endif()

# once_fn(): its shared object would carry strong_once() strong, so refused -- a failure
# marker in the store, no object, and the unit's own piece instead.
store_key("${WORKDIR}/a.o.split/include/once.h_*_once_fn.cpp" once_key)
file(GLOB once_objs "${WORKDIR}/store/${once_key}-*.o")
if(once_objs)
  message(FATAL_ERROR "the store holds an object for once_fn(), which carries strong_once() every includer would then define: ${once_objs}")
endif()
file(GLOB once_fail "${WORKDIR}/store/${once_key}-*.o.fail")
if(NOT once_fail)
  message(FATAL_ERROR "no refusal marker for once_fn()'s shared piece in the store")
endif()
if(NOT a_log MATCHES "may exist once only[^\n]*once\\.h")
  message(FATAL_ERROR "a.cpp did not refuse once_fn()'s shared piece for its strong symbol:\n${a_log}")
endif()
file(GLOB once_twin_obj "${WORKDIR}/a.o.split/include/once.h_*_once_fn.o")
if(NOT once_twin_obj)
  message(FATAL_ERROR "a.cpp did not compile its own piece for once_fn()")
endif()
message(STATUS "shared_fn() shared once, once_fn() refused for strong_once(): 117 15")
