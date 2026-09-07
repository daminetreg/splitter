# Where a variable that may exist in only one object is put, and whether the splitter says so.
#
# Asserting on the *placement* rather than on the program's behaviour is deliberate. The
# consequence of getting it wrong -- a dynamic initialiser running in link order relative to
# code that reads it during static initialisation -- is real but not deterministic: a link
# whose order happens to be favourable produces a correct program from wrong placement, so a
# runtime assertion here passes with and without the fix. The placement itself is decided by
# the splitter and is exactly what changed.

foreach(required SPLITTER SOURCE WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

get_filename_component(source_name "${SOURCE}" NAME)
get_filename_component(stem "${SOURCE}" NAME_WE)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)

function(split_at std out_log out_preamble out_defs)
  set(object "${WORKDIR}/${std}.o")
  file(REMOVE_RECURSE "${object}.split")
  execute_process(
    COMMAND   "${SPLITTER}" "${CXX}" "-std=${std}" "-I${WORKDIR}"
              -c -o "${object}" "${WORKDIR}/${source_name}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE res)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "${std}: launcher failed (${res}):\n${out}${err}")
  endif()
  if("${out}${err}" MATCHES "falling back")
    message(FATAL_ERROR "${std}: cpp-splitter fell back, so nothing was placed:\n${out}${err}")
  endif()

  set(preamble "${object}.split/${stem}_preamble.h")
  if(NOT EXISTS "${preamble}")
    message(FATAL_ERROR "${std}: no preamble at ${preamble}")
  endif()
  file(READ "${preamble}" preamble_text)

  set(defs_text "")
  file(GLOB defs "${object}.split/*_definitions.h")
  foreach(d IN LISTS defs)
    file(READ "${d}" one)
    string(APPEND defs_text "${one}")
  endforeach()

  set(${out_log} "${out}${err}" PARENT_SCOPE)
  set(${out_preamble} "${preamble_text}" PARENT_SCOPE)
  set(${out_defs} "${defs_text}" PARENT_SCOPE)
endfunction()

# --- C++17: the definition stays put, marked inline, and nothing is said -------------------
split_at("c++17" log17 preamble17 defs17)

if(NOT preamble17 MATCHES "inline[ \t]+std::string[ \t]+param_name")
  message(FATAL_ERROR
    "c++17: the preamble should keep param_name in place as an inline variable:\n${preamble17}")
endif()
if(defs17 MATCHES "param_name[ \t]*=")
  message(FATAL_ERROR
    "c++17: param_name was moved to the definitions header anyway:\n${defs17}")
endif()
if(log17 MATCHES "cpp-splitter. warning")
  message(FATAL_ERROR "c++17: nothing to warn about, but it warned:\n${log17}")
endif()

# --- C++14: inline variables do not exist, so it moves -- and says so ----------------------
split_at("c++14" log14 preamble14 defs14)

if(NOT defs14 MATCHES "param_name[ \t]*=")
  message(FATAL_ERROR
    "c++14: param_name should have moved to the definitions header:\n${defs14}")
endif()
if(NOT preamble14 MATCHES "extern[ \t]+std::string[ \t]+param_name")
  message(FATAL_ERROR
    "c++14: the preamble should keep an extern declaration in its place:\n${preamble14}")
endif()
# Matched in pieces: CMake's `.` does not cross the line breaks in the captured output. The
# warning names whichever variable is moved first, which is not necessarily param_name, so the
# name is not part of the assertion.
if(NOT (log14 MATCHES "warning" AND log14 MATCHES "moved to another object"
        AND log14 MATCHES "link order"))
  message(FATAL_ERROR
    "c++14: moving it is a hazard the user has to be told about, and was not:\n${log14}")
endif()

message(STATUS "ok: inline in place at C++17, moved with a warning at C++14")
