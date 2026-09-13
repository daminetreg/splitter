#!/usr/bin/env bash
#
# The five scenarios, run through CMake RE against EngFlow's Remote Build Execution cluster,
# on a minimal OpenCV (core and imgproc), with and without cpp-splitter in front of the
# compiler -- the OpenCV-shaped twin of benchmark-spirit-cmake-re.sh. Scenario definitions,
# columns, MODES, REMOTE_SPLIT and the build-only timing are the same; what differs is the
# corpus and the three files the scenarios touch:
#
#   one source   modules/core/src/arithm.cpp
#   one header   opencv2/core/cvdef.h, which every unit includes
#   one body     the body of SparseMat::nzcount() in opencv2/core/mat.inl.hpp -- a header
#                mirrored into all 158 unit split directories, and a function emitted by 1
#                of them (measured from the split tree, see benchmarks/opencv-local-split.md)
#
# Usage:
#   ./benchmark-opencv-cmake-re.sh                                  # distributed
#   ./benchmark-opencv-cmake-re.sh --host                           # this machine only
#   BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-opencv-cmake-re.sh
#   REMOTE_SPLIT=1 ./benchmark-opencv-cmake-re.sh                   # split on the cluster too
#   REMOTE_SPLIT=1 MODES=remote ./benchmark-opencv-cmake-re.sh      # only the cluster-split rows
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENCV="$REPO/example/opencv"

MODE="${1:---distributed}"
case "$MODE" in
    --distributed|--host) ;;
    *) echo "usage: $0 [--distributed|--host]" >&2; exit 2 ;;
esac
DRIVER="$REPO/build-opencv-cmake-re.sh"

SOURCE="$OPENCV/modules/core/src/arithm.cpp"
HEADER="$OPENCV/modules/core/include/opencv2/core/cvdef.h"
BODY_HEADER="$OPENCV/modules/core/include/opencv2/core/mat.inl.hpp"
BODY_BACKUP="$(mktemp)"
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP" "${PEAK_FILE:-}"' EXIT

patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "size_t SparseMat::nzcount() const\n{\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "    (void)%s;  // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PROBE
}

# Peak resident memory held by this benchmark's own processes, sampled rather than taken from
# ru_maxrss: that reports the largest single child, and the question at a high job count is what
# the machine has to hold at once when several hundred launchers each carry a libclang AST.
#
# Attribution is by process group, not by name. A system-wide total is meaningless here -- this
# machine has other tenants, and a filter on `clang` or `ninja` picks up their builds too, tens
# of gigabytes of them. A non-interactive shell puts every descendant in its own process group,
# so field 5 of /proc/<pid>/stat identifies ours; field 24 is its resident set, in pages.
BENCH_PGID="$(ps -o pgid= -p $$ | tr -d ' ')"

sample_rss() {   # sample_rss <output file>; runs until killed
    # Deliberately outside `set -e`: processes come and go while awk is reading /proc, so awk
    # regularly exits non-zero having printed a perfectly good total, and a failing sample must
    # not end the sampler. Not doing this reported 0 for every row the first time it ran.
    set +e
    local peak=0 total
    while :; do
        total=$(awk -v pg="$BENCH_PGID" '
            {
                line = $0
                sub(/^[0-9]+ \(.*\) /, "", line)     # comm can hold spaces and parentheses
                split(line, f, " ")
                if (f[3] == pg) { pages += f[22] }
            }
            END { print pages * 4 }                   # 4 kB pages
        ' /proc/[0-9]*/stat 2>/dev/null | tail -1)
        case "$total" in ''|*[!0-9]*) total=0 ;; esac
        if [ "$total" -gt "$peak" ]; then
            peak="$total"
        fi
        echo "$peak" > "$1"
        sleep 2
    done
}
human_rss() { awk -v v="$1" 'BEGIN { printf "%6.1fG", v/1048576 }'; }

human() { awk -v v="$1" 'BEGIN { printf "%7.1fs", v/1000 }'; }

# Reclient keeps no record of where an action ran unless it is asked to, so build-opencv-cmake-re.sh
# points RBE_proxy_log_dir here. Read the records rather than trusting the wall clock.
#
# The wall column is the build only. Peak RSS still spans the whole invocation, which costs
# nothing in accuracy: mirroring and configuring are a couple of single-threaded processes
# against several hundred concurrent launchers.
actions() {
    local files=( "$REPO"/build/rbe-logs/*.rrpl )
    if [ ! -e "${files[0]}" ]; then printf 'none'; return; fi
    local r c l
    r=$(grep -aoE 'REMOTE_EXECUTION' "${files[@]}" 2>/dev/null | wc -l)
    c=$(grep -aoE 'CACHE_HIT' "${files[@]}" 2>/dev/null | wc -l)
    l=$(grep -aoE 'LOCAL_FALLBACK' "${files[@]}" 2>/dev/null | wc -l)
    printf '%s/%s/%s' "$r" "$c" "$l"
}

# The peak is passed between run() and report() through this file rather than a variable.
# run() is called in a command substitution -- `t=$(run ...)` -- so it executes in a subshell
# and any variable it sets is discarded when that subshell exits. Assigning PEAK there reported
# 0.0G for every row of a full benchmark before this was noticed.
PEAK_FILE="$(mktemp)"
echo 0 > "$PEAK_FILE"

run() {  # run <log> <extra args...> -> the build's elapsed ms; peak RSS lands in $PEAK_FILE
    local log="$1"; shift
    rm -rf "$REPO/build/rbe-logs"
    local sampler
    echo 0 > "$PEAK_FILE"
    sample_rss "$PEAK_FILE" & sampler=$!
    "$DRIVER" "$MODE" "$@" > "$log" 2>&1 || { kill "$sampler" 2>/dev/null; echo "BUILD FAILED, see $log" >&2; exit 1; }
    kill "$sampler" 2>/dev/null || true
    # A build the OOM killer got into is not a measurement. At high -j the splitter holds a
    # libclang AST per unit alongside everything else, and the victims look like defects.
    if grep -q '^Killed\|signal 9' "$log" 2>/dev/null; then
        echo "OOM kills during $log -- this is not a measurement" >&2
        exit 1
    fi
    # The build alone, as the driver timed it. Copying the project in, mirroring the sources
    # and configuring are real cost but they are not the build, and they were adding about 20s
    # to every row -- enough that the no-op row was almost entirely them.
    #
    # A missing marker is an error rather than a fallback to the whole invocation: the two
    # differ by that 20s, and silently reporting one as the other is exactly the confusion this
    # removes.
    local ms_line
    ms_line="$(sed -n 's/^==> build wall ms: \([0-9]\+\)$/\1/p' "$log" | tail -1)"
    if [ -z "$ms_line" ]; then
        echo "no '==> build wall ms:' line in $log -- the driver did not report a build time" >&2
        exit 1
    fi
    echo "$ms_line"
}

fallbacks() { grep -c 'falling back' "$1" 2>/dev/null || true; }

echo "==> mode: $MODE   build type: ${BUILD_TYPE:-Debug}   jobs: ${CMAKE_RE_JOBS:-default}"
echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j"$(nproc)" >/dev/null

printf '\n%-12s %-8s %9s %10s %-18s %8s\n' scenario splitter wall fallbacks 'remote/cached/local' 'peak RSS'
printf '%-12s %-8s %9s %10s %-18s %8s\n' ------------ -------- --------- ---------- ------------------ --------
report() { printf '%-12s %-8s %9s %10s %-18s %8s\n' "$1" "$2" "$(human "$3")" "$4" "$5" "$(human_rss "$(cat "$PEAK_FILE")")"; }

# Three configurations rather than two when REMOTE_SPLIT is set, in one run: the point of
# TODO/35 is splitting here against splitting on the cluster, and comparing those across two
# runs would compare cluster cache states as much as anything else.
#
# MODES narrows that when only one configuration is in question and a full pass is not worth
# half an hour -- MODES=remote measures the cluster-split rows alone. Numbers from a narrowed
# run are comparable within themselves and should be reported as their own set, for the reason
# just given.
modes=(plain split)
if [ "${REMOTE_SPLIT:-0}" = 1 ]; then
    modes=(plain split remote)
fi
if [ -n "${MODES:-}" ]; then
    read -r -a modes <<< "$MODES"
    for m in "${modes[@]}"; do
        case "$m" in
            plain|split|remote) ;;
            *) echo "MODES may name plain, split and remote; got '$m'" >&2; exit 2 ;;
        esac
    done
fi

for mode in "${modes[@]}"; do
    args=(); label=no
    case "$mode" in
        split)  args=(--split); label=yes ;;
        remote) args=(--split --remote-split); label=cluster ;;
    esac
    L=/tmp/bench-opencv-cmake-re-$mode

    t=$(run "$L-full.log" --clean "${args[@]}")
    report full "$label" "$t" "$(fallbacks "$L-full.log")" "$(actions)"

    t=$(run "$L-noop.log" "${args[@]}")
    report no-op "$label" "$t" "$(fallbacks "$L-noop.log")" "$(actions)"

    touch "$SOURCE"
    t=$(run "$L-source.log" "${args[@]}")
    report "one source" "$label" "$t" "$(fallbacks "$L-source.log")" "$(actions)"

    touch "$HEADER"
    t=$(run "$L-header.log" "${args[@]}")
    report "one header" "$label" "$t" "$(fallbacks "$L-header.log")" "$(actions)"

    cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body "$((RANDOM))"
    t=$(run "$L-body.log" "${args[@]}")
    report "one body" "$label" "$t" "$(fallbacks "$L-body.log")" "$(actions)"
    cp "$BODY_BACKUP" "$BODY_HEADER"
done

echo
echo "==> remote/cached/local is REMOTE_EXECUTION / CACHE_HIT / LOCAL_FALLBACK, from reclient's"
echo "    own records. A row that is all cache hits did not compile anything on the cluster."
