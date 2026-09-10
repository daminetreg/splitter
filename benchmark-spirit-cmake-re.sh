#!/usr/bin/env bash
#
# The five scenarios, run through CMake RE against EngFlow's Remote Build Execution cluster,
# with and without cpp-splitter in front of the compiler.
#
# This is the question the other benchmarks cannot answer. They measure one machine, where
# splitting a translation unit into hundreds of pieces is strictly more work; the argument for
# doing it is that the pieces are independent and a build farm can take them all at once, which
# only shows up when there is a farm. Here there is one.
#
# Scenario definitions are the same as ./benchmark-spirit-tests.sh, deliberately, so the two
# can be read side by side:
#
#   full         one object per function, cold. --clean, because removing -B is not enough:
#                cmake-re keys its real build directory on the configuration, so an identical
#                configure lands back on the same one and ninja reports "no work to do" over
#                the previous run's outputs.
#   no-op        a settled tree should do nothing.
#   one source   touch one test .cpp -- timestamp only, content identical.
#   one header   touch a widely included header -- timestamp only, content identical.
#   one body     change the body of standard_wide::toucs4(), an ordinary non-template function
#                in a header 194 of the units include and exactly 1 of them emits.
#
# That last choice is the point of this benchmark rather than a detail. A plain build has to
# recompile every unit that includes the header -- 194 of them -- because the header changed.
# A split build re-runs the launcher for all 194 too, but only the unit that actually emits the
# function has a piece to recompile: the other 193 keep the definition in their rewritten copy,
# and the splitter decides piece recompilation from the piece source and the preamble, neither
# of which moved. So the work that distributes drops from ~178 remote compiles, which is what
# editing utf8_put_encode costs, to one.
#
# Measured with utf8_put_encode -- emitted in 178 of the 180 units that include it -- the row
# was a wash: 210 remote actions against the plain build's 213. This is the same edit shaped to
# ask whether that was the splitter failing or the target failing to discriminate.
#
# The two touch rows are free by construction here and that is worth stating rather than
# hiding: cmake-re mirrors sources by content, so a file whose bytes did not change is not
# re-mirrored and nothing downstream runs. On a single machine those rows measure the
# splitter's own input hashing; here they measure nothing, which is the correct answer.
#
# Every row reports the RBE action counts, because wall time alone cannot tell a build that
# executed remotely from one that was served out of the cluster's cache -- and those differ by
# an order of magnitude. A row whose actions are all CACHE_HIT measured scheduling and local
# work, not compilation.
#
# Usage:
#   ./benchmark-spirit-cmake-re.sh                                  # distributed
#   ./benchmark-spirit-cmake-re.sh --host                           # this machine only
#   BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-spirit-cmake-re.sh
#   REMOTE_SPLIT=1 ./benchmark-spirit-cmake-re.sh      # split on the cluster too (TODO/35)
#
# REMOTE_SPLIT applies to the split rows only, and needs --distributed: it makes the launcher
# shell itself out through rewrapper instead of parsing here, which is the thing worth
# measuring at a job count where the parses are what exhausts the machine.
#
# BUILD_TYPE is part of every RBE action key, so switching it empties the cache from the
# cluster's point of view -- which is the only way from here to measure a genuinely cold
# distributed build on both sides.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOST="$REPO/example/boost-to-split"

MODE="${1:---distributed}"
case "$MODE" in
    --distributed|--host) ;;
    *) echo "usage: $0 [--distributed|--host]" >&2; exit 2 ;;
esac
DRIVER="$REPO/build-spirit-cmake-re.sh"

SOURCE="$BOOST/libs/spirit/test/qi/char1.cpp"
HEADER="$BOOST/libs/spirit/include/boost/spirit/home/support/char_encoding/standard.hpp"
BODY_HEADER="$BOOST/libs/spirit/include/boost/spirit/home/support/char_encoding/standard_wide.hpp"
BODY_BACKUP="$(mktemp)"
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP" "${PEAK_FILE:-}"' EXIT

patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "        toucs4(wchar_t ch)\n        {\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "            (void)%s;  // benchmark probe\n" % marker, 1)
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

ms() { date +%s%3N; }
human() { awk -v v="$1" 'BEGIN { printf "%7.1fs", v/1000 }'; }

# Reclient keeps no record of where an action ran unless it is asked to, so build-spirit-cmake-re.sh
# points RBE_proxy_log_dir here. Read the records rather than trusting the wall clock.
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

run() {  # run <log> <extra args...> -> elapsed ms; peak RSS lands in $PEAK_FILE
    local log="$1"; shift
    rm -rf "$REPO/build/rbe-logs"
    local start end sampler
    echo 0 > "$PEAK_FILE"
    sample_rss "$PEAK_FILE" & sampler=$!
    start=$(ms)
    "$DRIVER" "$MODE" "$@" > "$log" 2>&1 || { kill "$sampler" 2>/dev/null; echo "BUILD FAILED, see $log" >&2; exit 1; }
    end=$(ms)
    kill "$sampler" 2>/dev/null || true
    # A build the OOM killer got into is not a measurement. At high -j the splitter holds a
    # libclang AST per unit alongside everything else, and the victims look like defects.
    if grep -q '^Killed\|signal 9' "$log" 2>/dev/null; then
        echo "OOM kills during $log -- this is not a measurement" >&2
        exit 1
    fi
    echo $((end - start))
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
modes=(plain split)
if [ "${REMOTE_SPLIT:-0}" = 1 ]; then
    modes=(plain split remote)
fi

for mode in "${modes[@]}"; do
    args=(); label=no
    case "$mode" in
        split)  args=(--split); label=yes ;;
        remote) args=(--split --remote-split); label=cluster ;;
    esac
    L=/tmp/bench-cmake-re-$mode

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
