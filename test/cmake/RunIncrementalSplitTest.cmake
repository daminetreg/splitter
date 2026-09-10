# Splits a unit, edits one function body in a header, and splits again -- and asserts that the
# second split took the fast path and produced exactly what a full split would have produced.
#
# Two assertions, and the second is the one that matters. That the log says "re-sliced" only
# says the fast path ran; it says nothing about whether it was right. So every file the split
# directory holds is hashed after the incremental run, the cache is dropped, the same edit is
# split again with CPP_SPLITTER_NO_INCREMENTAL_SPLIT=1, and the two sets of hashes must agree.
# A fast path that is merely fast is a defect.
#
# The guards get their own runs. Each makes an edit the fast path must refuse, and asserts on
# the reason it gives: a guard that silently stops working would leave the byte-identity check
# above passing, because it would simply never be exercised.

foreach(required SPLITTER SOURCE HEADER WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(header_name "${HEADER}" NAME)
get_filename_component(source_name "${SOURCE}" NAME)
configure_file("${HEADER}" "${WORKDIR}/${header_name}" COPYONLY)
configure_file("${SOURCE}" "${WORKDIR}/${source_name}" COPYONLY)

set(object "${WORKDIR}/unit.o")

# Runs the launcher on the same object path every time, which is what makes a later run reuse
# the earlier one's split directory.
#
# -MD is not decoration. split_inputs_hash() hashes the prerequisites recorded in
# depfile.cache, and without dependency flags there is no depfile, no cache and nothing for
# the fast path to compare against -- the split would simply be redone every time. A build
# system always passes these, so passing them here is the honest configuration.
function(split label out_log)
  if(DEFINED ARGV2 AND ARGV2)
    set(extra_env "CPP_SPLITTER_NO_INCREMENTAL_SPLIT=1")
  else()
    set(extra_env "CPP_SPLITTER_QUIET_UNUSED=0")
  endif()
  execute_process(
    COMMAND   "${CMAKE_COMMAND}" -E env CPP_SPLITTER_VERBOSE=1 ${extra_env}
              "${SPLITTER}" "${CXX}" "-I${WORKDIR}"
              -MD -MF "${WORKDIR}/unit.d" -MT "${object}"
              -c -o "${object}" "${WORKDIR}/${source_name}"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(log "${o}${e}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: launcher failed (${rc}):\n${log}")
  endif()
  if(log MATCHES "fallback|falling back")
    message(FATAL_ERROR "${label}: cpp-splitter fell back instead of splitting:\n${log}")
  endif()
  set(${out_log} "${log}" PARENT_SCOPE)
endfunction()

function(check_program expected label)
  set(program "${WORKDIR}/prog_${label}")
  execute_process(COMMAND "${CXX}" -o "${program}" "${object}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: linking failed (${rc}):\n${o}${e}")
  endif()
  execute_process(COMMAND "${program}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  string(STRIP "${out}" out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: the program failed (${rc}), printed '${out}'")
  endif()
  if(NOT out STREQUAL expected)
    message(FATAL_ERROR "${label}: expected '${expected}', got '${out}'")
  endif()
endfunction()

# Every generated file in the split directory, hashed. Objects are excluded: they are the
# compiler's output, not the splitter's, and comparing them would test the compiler.
function(snapshot out_var)
  file(GLOB_RECURSE files "${object}.split/*")
  list(SORT files)
  set(acc "")
  foreach(f ${files})
    if(NOT IS_DIRECTORY "${f}" AND NOT f MATCHES "\\.(o|gch|pch)$" AND NOT f MATCHES "/split\\.cache$")
      file(MD5 "${f}" h)
      file(RELATIVE_PATH rel "${object}.split" "${f}")
      string(APPEND acc "${rel} ${h}\n")
    endif()
  endforeach()
  set(${out_var} "${acc}" PARENT_SCOPE)
endfunction()

split(first log)
check_program("7 42" first)

# The edit under test: one more line inside value()'s body, which both changes that body and
# moves every line after it -- twice()'s #line directive included.
file(READ "${WORKDIR}/${header_name}" text)
string(REPLACE "        return 3 + 4;"
               "        int bump = 100;\n        return 3 + 4 + bump;" text "${text}")
file(WRITE "${WORKDIR}/${header_name}" "${text}")

split(incremental log)
if(NOT log MATCHES "re-sliced its piece without parsing")
  message(FATAL_ERROR
    "the fast path did not run, so this test is not covering what it was written for:\n${log}")
endif()
check_program("107 42" incremental)
snapshot(after_incremental)

# Same edit, same inputs, but forced through a full parse. Anything the fast path got wrong
# shows up here.
file(REMOVE "${object}.split/split.cache")
split(full log TRUE)
snapshot(after_full)

if(NOT after_incremental STREQUAL after_full)
  message(FATAL_ERROR
    "the fast path and a full split disagree.\nincremental:\n${after_incremental}\n"
    "full:\n${after_full}")
endif()
check_program("107 42" full)

# --- the guards ---------------------------------------------------------------------------

function(expect_refusal reason label)
  split("${label}" log)
  if(NOT log MATCHES "full split: ${reason}")
    message(FATAL_ERROR "${label}: expected a refusal matching '${reason}', got:\n${log}")
  endif()
endfunction()

# Settle first: the guard runs below must each be the only change since the previous split.
file(READ "${WORKDIR}/${header_name}" baseline)

file(WRITE "${WORKDIR}/${header_name}" "${baseline}")
string(REPLACE "        int bump = 100;"
               "#define INCREMENTAL_PROBE 1\n        int bump = 100;" text "${baseline}")
file(WRITE "${WORKDIR}/${header_name}" "${text}")
expect_refusal("the new body contains a preprocessor directive" directive)

file(WRITE "${WORKDIR}/${header_name}" "${baseline}")
split(settle log)
string(REPLACE "inline int value() const" "inline int value(int unused_arg) const"
       text "${baseline}")
file(WRITE "${WORKDIR}/${header_name}" "${text}")
expect_refusal("the definition's signature changed" signature)

message(STATUS "ok: a body-only edit is re-sliced, matches a full split, and the guards refuse")

# --- a definition this unit keeps rather than emits ----------------------------------------
#
# The header holds a function nothing here calls. No piece is emitted for it, so it stays in
# the unit's rewritten copy of the header -- and an edit to its body has to be re-sliced by
# patching that copy. Getting this wrong is not a correctness bug, because refusing falls back
# to a full split, which is right but pays a parse.

file(WRITE "${WORKDIR}/${header_name}" "${baseline}")
split(settle_kept log)

# The edit adds a line, so everything below it moves. That matters here beyond the usual
# renumbering: the guard against a kept definition sitting inside the edited body reads the
# `.keeps` file, and a kept definition that is *itself* the one being edited is listed there at
# its own start line. Reading that as "inside" made the fast path refuse its own work, on 112
# of Boost.Spirit's units. An edit that keeps the line count would not notice -- the guard only
# runs when something moved.
file(READ "${WORKDIR}/${header_name}" kept_text)
string(REPLACE "    return 11;" "    int kept_bump = 1;\n    return 11 + kept_bump;"
       kept_text "${kept_text}")
file(WRITE "${WORKDIR}/${header_name}" "${kept_text}")

split(kept log)
if(NOT log MATCHES "re-sliced its piece without parsing")
  message(FATAL_ERROR
    "an edit to a body this unit keeps in its copy of the header was not re-sliced:\n${log}")
endif()
check_program("107 42" kept)
snapshot(after_kept)

file(REMOVE "${object}.split/split.cache")
split(kept_full log TRUE)
snapshot(after_kept_full)

if(NOT after_kept STREQUAL after_kept_full)
  message(FATAL_ERROR
    "re-slicing a kept definition disagrees with a full split.\nre-sliced:\n${after_kept}\n"
    "full:\n${after_kept_full}")
endif()

message(STATUS "ok: a definition kept in the header copy is re-sliced too, and matches")
