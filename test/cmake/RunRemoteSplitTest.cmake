# Produce the split on the Remote Build Execution cluster. TODO/35.
#
# This test talks to a real cluster on purpose. A stub `rewrapper` would only prove that we
# format a command line the way we meant to; it cannot show that the inputs we declared were
# sufficient, that the outputs came home, or that a sandbox reproduces what a local split
# produces -- which are the three things that can actually be wrong.
#
# It SKIPS only where there is nothing to talk to -- no credentials, no cmake-re. Everything
# after that is a failure, including a configure that does not complete: a skip that can absorb
# a real defect is worse than no test.
#
# WORKDIR must be outside any git repository. cmake-re mirrors the repository enclosing its
# source tree, so a probe project under this one makes it clone all 41176 files of it and then
# fail to find the sources.

foreach(required SPLITTER PROJECTDIR WORKDIR CXX TOOLCHAIN STAGE_DRIVERS)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

# Deliberately unmistakable: ctest decides this test skipped by matching this string in the
# output, and a plain "SKIP:" is a substring of things the cluster and CMake print on their own.
set(skip_marker "cpp-splitter-test-skip:")

# --- can this run at all? -----------------------------------------------------------------

if(NOT DEFINED ENV{ENGFLOW_MTLS_DIR})
  set(mtls "$ENV{HOME}/engflow-mTLS")
else()
  set(mtls "$ENV{ENGFLOW_MTLS_DIR}")
endif()
if(NOT EXISTS "${mtls}/engflow.crt" OR NOT EXISTS "${mtls}/engflow.key")
  message(STATUS "${skip_marker} no mTLS credentials in ${mtls}")
  message(STATUS "      set ENGFLOW_MTLS_DIR to a directory holding engflow.crt and engflow.key")
  return()
endif()

# CMAKE_RE_DIR first and on its own: `find_program(... PATHS)` appends, so it would have found
# whichever cmake-re is on PATH -- and only from v0.0.88 does cmake-re chain a user-specified
# CMAKE_CXX_COMPILER_LAUNCHER ahead of its own driver. The v0.0.87 in the image silently drops
# it, which builds and tests nothing.
find_program(cmake_re NAMES cmake-re PATHS "${CMAKE_RE_DIR}" NO_DEFAULT_PATH NO_CACHE)
if(NOT cmake_re)
  find_program(cmake_re NAMES cmake-re NO_CACHE)
endif()
if(NOT cmake_re)
  message(STATUS "${skip_marker} no cmake-re binary found")
  return()
endif()

# --- run ----------------------------------------------------------------------------------

# -B is a symlink into cmake-re's mirror, and cmake-re keys the directory it points at on the
# configuration -- so removing WORKDIR alone leaves a configured build tree that the next run
# adopts. That is how this test once built without the launcher at all: an earlier configure
# had populated the same directory. Resolve the link before wiping it, and take the real one.
if(EXISTS "${WORKDIR}/build")
  file(REAL_PATH "${WORKDIR}/build" mirror_build)
  if(IS_DIRECTORY "${mirror_build}")
    file(REMOVE_RECURSE "${mirror_build}")
  endif()
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/src" "${WORKDIR}/logs")
file(GLOB probe_files "${PROJECTDIR}/*")
foreach(f ${probe_files})
  get_filename_component(n "${f}" NAME)
  configure_file("${f}" "${WORKDIR}/src/${n}" COPYONLY)
endforeach()

# --- the image's drivers are not executable by us -----------------------------------------
#
# cmake-re compiles through `tipi-compiler-driver` and friends, found on PATH by bare name. The
# image ships them mode `-rwxrw-r--` owned by `tipi`, so running as our own uid with
# `--group-add tipi` we get the group bits and every one of them fails with
# `/bin/sh: 1: tipi-compiler-driver: Permission denied`. Take copies we own instead. Shared with
# build-spirit-cmake-re.sh so there is one description of the workaround, not two.
execute_process(
  COMMAND "${STAGE_DRIVERS}" "${WORKDIR}/drivers"
  OUTPUT_VARIABLE shim_dir OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE shim_err RESULT_VARIABLE shim_rc)
if(NOT shim_rc EQUAL 0)
  message(FATAL_ERROR "could not stage the tipi drivers: ${shim_err}")
endif()
if(shim_dir)
  message(STATUS "staged the tipi drivers into ${shim_dir} (not group-executable in the image)")
  set(path_with_drivers "${shim_dir}:$ENV{PATH}")
else()
  set(path_with_drivers "$ENV{PATH}")
endif()

set(common_env
  "TIPI_DISABLE_AR_RANLIB_DRIVER=ON" "TIPI_CACHE_CONSUME_ONLY=ON" "TIPI_CACHE_FORCE_ENABLE=OFF"
  "RBE_service=$ENV{RBE_service}" "RBE_service_no_auth=true"
  "RBE_tls_client_auth_cert=${mtls}/engflow.crt"
  "RBE_tls_client_auth_key=${mtls}/engflow.key"
  "RBE_proxy_log_dir=${WORKDIR}/logs"
  "CPP_SPLITTER_VERBOSE=1"
  "PATH=${path_with_drivers}")
if("$ENV{RBE_service}" STREQUAL "")
  list(APPEND common_env "RBE_service=opal.cluster.engflow.com:443")
endif()

function(run_build label remote out_log)
  set(env ${common_env})
  if(remote)
    list(APPEND env "CPP_SPLITTER_REMOTE_SPLIT=1")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${env}
            "${cmake_re}" --build "${WORKDIR}/build" --host --distributed -j8
    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  file(WRITE "${WORKDIR}/${label}.log" "${o}${e}")
  set(${out_log} "${o}${e}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label}: build failed (${rc}), see ${WORKDIR}/${label}.log")
  endif()
endfunction()

# Hash every generated file the splitter is responsible for. Objects are the compiler's output
# and are excluded; so are the caches, which legitimately differ between a local and a remote
# split because they record how the split was obtained.
# Note the glob is rooted at the resolved mirror directory and filters afterwards. GLOB_RECURSE
# only recurses below the last slash of the pattern, so "CMakeFiles/*.split/*" quietly matched
# nothing: the split directories sit two levels down, in CMakeFiles/<target>.dir/.
function(split_outputs out_var)
  file(GLOB_RECURSE all "${mirror_build}/CMakeFiles/*")
  set(picked "")
  foreach(f ${all})
    if(f MATCHES "\\.split/")
      list(APPEND picked "${f}")
    endif()
  endforeach()
  set(${out_var} "${picked}" PARENT_SCOPE)
endfunction()

function(snapshot out_var)
  split_outputs(files)
  list(SORT files)
  set(acc "")
  foreach(f ${files})
    if(NOT IS_DIRECTORY "${f}"
       AND NOT f MATCHES "\\.(o|gch|pch|log)$"
       AND NOT f MATCHES "/(split\\.cache|depfile\\.cache|inputs\\.hash)$")
      file(MD5 "${f}" h)
      get_filename_component(n "${f}" NAME)
      string(APPEND acc "${n} ${h}\n")
    endif()
  endforeach()
  set(${out_var} "${acc}" PARENT_SCOPE)
endfunction()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${common_env}
          "${cmake_re}" --host --distributed -S "${WORKDIR}/src" -B "${WORKDIR}/build"
          -DCMAKE_BUILD_TYPE=Release
          "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}"
          "-DCMAKE_CXX_COMPILER_LAUNCHER=${SPLITTER}"
  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
file(WRITE "${WORKDIR}/configure.log" "${o}${e}")
# A configure failure is a failure. Reporting it as a skip once hid the actual defect for a
# full run: the work directory had been placed inside this repository, so cmake-re mirrored the
# whole of it -- 41176 files -- and then could not find the sources.
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "cmake-re could not configure the probe project. See ${WORKDIR}/configure.log; the last "
    "line usually says it. Note that WORKDIR must be outside any git repository, because "
    "cmake-re mirrors the repository enclosing its source tree.")
endif()

# The launcher has to be in the cache, chained ahead of cmake-re's own compiler driver. If it
# is not, everything below still passes a build and proves nothing about cpp-splitter.
file(REAL_PATH "${WORKDIR}/build" mirror_build)
file(STRINGS "${mirror_build}/CMakeCache.txt" launcher_line REGEX "^CMAKE_CXX_COMPILER_LAUNCHER")
if(NOT launcher_line MATCHES "${SPLITTER}")
  message(FATAL_ERROR
    "cmake-re did not configure cpp-splitter as the C++ compiler launcher, so the build below "
    "would exercise nothing.\n"
    "  cmake-re:   ${cmake_re}\n"
    "  cache says: ${launcher_line}\n"
    "  expected it to mention: ${SPLITTER}\n"
    "Chaining a user launcher ahead of tipi-compiler-driver needs cmake-re v0.0.88 or newer.")
endif()

run_build(remote TRUE remote_log)

if(remote_log MATCHES "fallback|falling back")
  message(FATAL_ERROR "the build fell back to plain compilation:\n${remote_log}")
endif()
if(NOT remote_log MATCHES "remote split: [0-9]+ piece\\(s\\) returned from the cluster")
  message(FATAL_ERROR
    "the split was not produced on the cluster. This test exists to cover exactly that, so a "
    "local split here is a failure, not a pass. See ${WORKDIR}/remote.log")
endif()

# What reclient recorded. A second run of an unchanged action is served from the cluster's
# action cache, so CACHE_HIT is as good as REMOTE_EXECUTION here -- both mean the split was
# produced on the cluster rather than computed on this machine. What must not appear is a local
# execution or a local fallback, which is the failure this test exists to catch.
file(GLOB records "${WORKDIR}/logs/*.rrpl")
if(NOT records)
  message(FATAL_ERROR "reclient wrote no action records to ${WORKDIR}/logs")
endif()
set(remote_actions 0)
set(local_actions 0)
foreach(r ${records})
  file(STRINGS "${r}" status_lines REGEX "status: (REMOTE_EXECUTION|CACHE_HIT|LOCAL_EXECUTION|LOCAL_FALLBACK)")
  foreach(line ${status_lines})
    if(line MATCHES "LOCAL")
      math(EXPR local_actions "${local_actions} + 1")
    else()
      math(EXPR remote_actions "${remote_actions} + 1")
    endif()
  endforeach()
endforeach()
if(local_actions GREATER 0)
  message(FATAL_ERROR
    "${local_actions} action(s) ran locally or fell back to local. The split has to be produced "
    "on the cluster for this test to mean anything. See ${WORKDIR}/logs")
endif()
if(remote_actions EQUAL 0)
  message(FATAL_ERROR "reclient's records show no remote action at all; see ${WORKDIR}/logs")
endif()
message(STATUS "${remote_actions} action(s) served by the cluster, none locally")

snapshot(after_remote)
if(after_remote STREQUAL "")
  message(FATAL_ERROR "no split output found; the snapshot would compare nothing")
endif()

# Removing the split trees and the objects is what makes the build system run the launcher
# again -- and, where a phase is about what a remote split leaves behind, what stops an earlier
# local build's leftovers from standing in for it.
function(clear_split_trees)
  file(GLOB_RECURSE entries LIST_DIRECTORIES true "${mirror_build}/CMakeFiles/*")
  foreach(d ${entries})
    if(IS_DIRECTORY "${d}" AND d MATCHES "\\.split$")
      file(REMOVE_RECURSE "${d}")
    elseif(d MATCHES "\\.o$" OR d MATCHES "\\.o\\.d$")
      # The .o.d goes too. rewrite_depfile() reads it when it is there, so one left over from
      # an earlier phase supplies the prerequisite list that a remote split is supposed to
      # bring home itself -- and the phase below then passes whether or not it does.
      file(REMOVE "${d}")
    endif()
  endforeach()
endfunction()

# Same sources, split here instead.
clear_split_trees()

run_build(local FALSE local_log)
snapshot(after_local)

if(NOT after_remote STREQUAL after_local)
  message(FATAL_ERROR
    "a split produced on the cluster differs from one produced here.\nremote:\n${after_remote}\n"
    "local:\n${after_local}")
endif()

message(STATUS "ok: the cluster produced the split, and it matches a local one byte for byte")

# --- a remote split over a populated split directory --------------------------------------
#
# Every phase above starts from an empty split directory, and that is the case that works.
# reclient *merges* an -output_directories result into what is already there, so a second
# remote split leaves behind every piece the new one did not overwrite. Piece names carry an
# index, so a unit whose set of definitions changed ends up holding two generations at once and
# `ld -r` reports `multiple definition of main`. The splitter then falls back to compiling the
# unit whole -- a build that succeeds and tests nothing, which is why this phase asserts on the
# absence of a fallback rather than on the exit code.
#
# Measured before the fix, on the `one body` row of Boost.Spirit's suite: 265 of 279 units.
file(APPEND "${WORKDIR}/src/shared.hpp" "
inline int shared_extra() { return 2; }
")

run_build(remote_again TRUE again_log)

if(again_log MATCHES "fallback|falling back")
  message(FATAL_ERROR
    "a second remote split over a populated split directory fell back to a plain compile. The "
    "directory has to be cleared before the action, because reclient merges rather than "
    "replaces.\n${again_log}")
endif()
if(NOT again_log MATCHES "remote split: [0-9]+ piece\\(s\\) returned from the cluster")
  message(FATAL_ERROR "the second split did not run on the cluster; see ${WORKDIR}/remote_again.log")
endif()

message(STATUS "ok: a remote split over a populated directory neither merges nor falls back")

# --- what a remote split leaves behind for the next edit ----------------------------------
#
# The invariant: after a split produced on the cluster, a one-line body edit must re-slice here
# and must not go back to the cluster. Re-slicing costs no parse and no network, and it rewrites
# only the piece whose body moved, so every other piece keeps its content and its action key.
#
# It holds because the prerequisite list survives a remote split, which is worth spelling out
# because it is not obvious: the launcher attaches `-MD -MF` to the unit's first *piece*
# compile, and the pieces are compiled here even when the split was not. So the compiler writes
# the unit's depfile as usual, rewrite_depfile() turns it into depfile.cache, and
# write_inputs_hashes() records what each prerequisite hashed to -- none of which knows or cares
# where the split came from.
#
# This phase starts from a tree only the cluster has populated, .o.d files included, because
# leftovers from the local phase above would supply that list even if a remote split did not.
clear_split_trees()
run_build(fresh_remote TRUE fresh_log)
if(NOT fresh_log MATCHES "remote split: [0-9]+ piece\\(s\\) returned from the cluster")
  message(FATAL_ERROR "expected a remote split into an empty tree; see ${WORKDIR}/fresh_remote.log")
endif()

split_outputs(produced)
set(with_hashes 0)
set(with_depfiles 0)
foreach(f ${produced})
  if(f MATCHES "/inputs\\.hash$")
    math(EXPR with_hashes "${with_hashes} + 1")
  elseif(f MATCHES "/depfile\\.cache$")
    math(EXPR with_depfiles "${with_depfiles} + 1")
  endif()
endforeach()
if(with_hashes EQUAL 0 OR NOT with_hashes EQUAL with_depfiles)
  message(FATAL_ERROR
    "a split produced on the cluster left ${with_hashes} inputs.hash against "
    "${with_depfiles} depfile.cache. Both are needed for the next edit to re-slice instead of "
    "going back to the cluster.")
endif()

# Change a body, and nothing else. The re-slice must handle it here, without the cluster.
file(READ "${WORKDIR}/src/shared.hpp" shared_text)
string(REPLACE "return 40;" "return 41;" shared_text "${shared_text}")
file(WRITE "${WORKDIR}/src/shared.hpp" "${shared_text}")

run_build(body_edit TRUE body_log)

if(body_log MATCHES "fallback|falling back")
  message(FATAL_ERROR "the body edit fell back to a plain compile:\n${body_log}")
endif()
if(NOT body_log MATCHES "one body changed in")
  message(FATAL_ERROR
    "a one-line body edit after a remote split did not re-slice. See ${WORKDIR}/body_edit.log")
endif()
if(body_log MATCHES "remote split: [0-9]+ piece\\(s\\) returned from the cluster")
  message(FATAL_ERROR
    "the body edit went back to the cluster for a full split. Re-slicing costs no parse and "
    "no network and has to win.\n${body_log}")
endif()

message(STATUS "ok: after a remote split, a body edit re-slices here instead of on the cluster")
