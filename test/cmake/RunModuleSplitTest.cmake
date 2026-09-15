# Splits a C++20 module interface unit through the launcher and checks what TODO/43 promises:
# the interface the compiler precompiles holds declarations and inline bodies only, each
# non-inline body is an implementation unit compiled against the BMI, an importer's pieces
# import the module themselves, and a body edit leaves the BMI byte-identical -- so the
# importer's launcher recompiles nothing -- while an inline body edit changes it and does.
#
# Skips with the suite's marker when the compiler cannot write a BMI (clang < 16).

foreach(required SPLITTER PROJECTDIR WORKDIR CXX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(src "${WORKDIR}/src")
file(COPY "${PROJECTDIR}/math.cppm" "${PROJECTDIR}/math_impl.cpp" "${PROJECTDIR}/use.cpp"
     DESTINATION "${src}")

# Can this compiler drive named modules the way CMake does?
file(WRITE "${WORKDIR}/probe.cppm" "export module probe;\nexport int one() { return 1; }\n")
execute_process(
  COMMAND "${CXX}" -std=c++20 -x c++-module -c "${WORKDIR}/probe.cppm"
          "-fmodule-output=${WORKDIR}/probe.pcm" -o "${WORKDIR}/probe.o"
  RESULT_VARIABLE probe_result OUTPUT_QUIET ERROR_QUIET)
if(NOT probe_result EQUAL 0)
  message("cpp-splitter-test-skip: ${CXX} cannot write a BMI (-fmodule-output), needs clang >= 16")
  return()
endif()
set(reduced "")
execute_process(
  COMMAND "${CXX}" -std=c++20 -x c++-module -fmodules-reduced-bmi -c "${WORKDIR}/probe.cppm"
          "-fmodule-output=${WORKDIR}/probe.pcm" -o "${WORKDIR}/probe.o"
  RESULT_VARIABLE reduced_result OUTPUT_QUIET ERROR_QUIET)
if(reduced_result EQUAL 0)
  set(reduced -fmodules-reduced-bmi)
endif()

set(pcm "${WORKDIR}/math.pcm")
set(ENV{CPP_SPLITTER_VERBOSE} 1)

# The three launcher calls a build makes, in dependency order. Each returns its log.
function(compile_interface out_log)
  execute_process(
    COMMAND "${SPLITTER}" "${CXX}" -std=c++20 -x c++-module ${reduced}
            "-fmodule-output=${pcm}" -MD -MF "${WORKDIR}/math.o.d" -MT "${WORKDIR}/math.o"
            -c -o "${WORKDIR}/math.o" "${src}/math.cppm"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "the interface unit did not build through the launcher (${r}):\n${o}${e}")
  endif()
  set(${out_log} "${o}${e}" PARENT_SCOPE)
endfunction()
function(compile_unit source object out_log)
  execute_process(
    COMMAND "${SPLITTER}" "${CXX}" -std=c++20 "-fmodule-file=math=${pcm}"
            -MD -MF "${object}.d" -MT "${object}" -c -o "${object}" "${source}"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "${source} did not build through the launcher (${r}):\n${o}${e}")
  endif()
  set(${out_log} "${o}${e}" PARENT_SCOPE)
endfunction()
function(link_and_run expected)
  execute_process(
    COMMAND "${CXX}" -o "${WORKDIR}/prog" "${WORKDIR}/math.o" "${WORKDIR}/math_impl.o" "${WORKDIR}/use.o"
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "link failed (${r}):\n${o}${e}")
  endif()
  execute_process(COMMAND "${WORKDIR}/prog" OUTPUT_VARIABLE out RESULT_VARIABLE r)
  string(STRIP "${out}" out)
  if(NOT r EQUAL 0 OR NOT out STREQUAL "${expected}")
    message(FATAL_ERROR "the program printed '${out}' (exit ${r}), expected '${expected}'")
  endif()
endfunction()
function(no_fallback log what)
  if(log MATCHES "falling back")
    message(FATAL_ERROR "${what} fell back:\n${log}")
  endif()
endfunction()

# 1. Cold: the three units through the launcher, then the program.
compile_interface(iface_log)
no_fallback("${iface_log}" "the interface unit")
compile_unit("${src}/math_impl.cpp" "${WORKDIR}/math_impl.o" impl_log)
if(NOT impl_log MATCHES "module implementation unit, compiling whole")
  message(FATAL_ERROR "the implementation unit was not declined as one:\n${impl_log}")
endif()
compile_unit("${src}/use.cpp" "${WORKDIR}/use.o" use_log)
no_fallback("${use_log}" "the importer")
link_and_run("42 7 14")

# 2. What the interface split wrote.
set(split "${WORKDIR}/math.o.split")
file(GLOB pieces RELATIVE "${split}" "${split}/math.cppm_*_*.cpp")
set(piece_names "${pieces}")
if(NOT piece_names MATCHES "helper_kept" OR NOT piece_names MATCHES "_seven")
  message(FATAL_ERROR "expected pieces for helper_kept and seven, got: ${piece_names}")
endif()
if(piece_names MATCHES "twice")
  message(FATAL_ERROR "the inline function got a piece: ${piece_names}")
endif()
file(READ "${split}/math_interface.cppm" interface)
if(NOT interface MATCHES "export[ \n]+int seven\\(\\);")
  message(FATAL_ERROR "the interface does not declare seven() in place of its body:\n${interface}")
endif()
if(NOT interface MATCHES "export[ \n]+inline int twice\\(int v\\) { return v \\* 2; }")
  message(FATAL_ERROR "the interface lost twice()'s inline body:\n${interface}")
endif()
if(interface MATCHES "return helper_kept\\(\\);")
  message(FATAL_ERROR "seven()'s body is still in the interface:\n${interface}")
endif()
file(GLOB seven_piece "${split}/math.cppm_*_seven.cpp")
file(READ "${seven_piece}" seven_text)
if(NOT seven_text MATCHES "module;\n#include \"math_preamble.h\"\nmodule math;")
  message(FATAL_ERROR "the piece is not an implementation unit replaying the fragment:\n${seven_text}")
endif()
# The importer's pieces restate the import and never mirror the interface unit as a header.
file(GLOB use_pieces "${WORKDIR}/use.o.split/use.cpp_*.cpp")
foreach(p ${use_pieces})
  file(READ "${p}" t)
  if(NOT t MATCHES "import math;")
    message(FATAL_ERROR "an importer piece lacks the import:\n${t}")
  endif()
endforeach()
file(GLOB_RECURSE mirrored "${WORKDIR}/use.o.split/include/*math.cppm*")
if(mirrored)
  message(FATAL_ERROR "the interface unit was mirrored as a header: ${mirrored}")
endif()

# 3. A non-inline body edit: the BMI is byte-identical, one piece recompiles, the importer
#    recompiles nothing.
file(SHA256 "${pcm}" pcm_before)
file(READ "${src}/math.cppm" text)
string(REPLACE "return helper_kept(); }" "return helper_kept() * 10; }" text "${text}")
file(WRITE "${src}/math.cppm" "${text}")
compile_interface(iface_log)
no_fallback("${iface_log}" "the interface unit after a body edit")
if(NOT iface_log MATCHES "BMI unchanged")
  message(FATAL_ERROR "the body edit rewrote the BMI:\n${iface_log}")
endif()
if(NOT iface_log MATCHES "compiling 1 split file|compile \\(seq\\)")
  message(FATAL_ERROR "expected exactly one piece to recompile:\n${iface_log}")
endif()
if(iface_log MATCHES "compile \\(interface\\)")
  message(FATAL_ERROR "the interface was recompiled after a body edit:\n${iface_log}")
endif()
file(SHA256 "${pcm}" pcm_after)
if(NOT pcm_before STREQUAL pcm_after)
  message(FATAL_ERROR "the BMI changed on a non-inline body edit")
endif()
compile_unit("${src}/math_impl.cpp" "${WORKDIR}/math_impl.o" impl_log)
compile_unit("${src}/use.cpp" "${WORKDIR}/use.o" use_log)
if(NOT use_log MATCHES "all up-to-date, skipping link")
  message(FATAL_ERROR "the importer did work after a body edit that left the BMI alone:\n${use_log}")
endif()
link_and_run("42 70 77")

# 4. An inline body edit: the BMI changes and the importer recompiles.
file(READ "${src}/math.cppm" text)
string(REPLACE "return v * 2; }" "return v + v; }" text "${text}")
file(WRITE "${src}/math.cppm" "${text}")
compile_interface(iface_log)
if(NOT iface_log MATCHES "BMI written")
  message(FATAL_ERROR "the inline body edit did not rewrite the BMI:\n${iface_log}")
endif()
file(SHA256 "${pcm}" pcm_inline)
if(pcm_inline STREQUAL pcm_after)
  message(FATAL_ERROR "the BMI did not change on an inline body edit")
endif()
compile_unit("${src}/math_impl.cpp" "${WORKDIR}/math_impl.o" impl_log)
compile_unit("${src}/use.cpp" "${WORKDIR}/use.o" use_log)
if(NOT use_log MATCHES "a BMI changed")
  message(FATAL_ERROR "the importer did not notice the BMI change:\n${use_log}")
endif()
link_and_run("42 70 77")
message(STATUS "module interface split: 42 7 14 -> 42 70 77, BMI stable across the body edit")
