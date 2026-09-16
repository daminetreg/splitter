#!/usr/bin/env bash
#
# Compare building part of Boost.Geometry's own test suite through cpp-splitter against
# building it normally.
#
# benchmark-geometry-split.sh measures a *consumer* of Boost.Geometry: code someone writes
# against the library. This measures the library's own tests, which is different in one way
# that turns out to dominate everything else: every Geometry test includes Boost.Test in
# header-only mode, and Boost.Test is unsplittable under the current design (TODO/24).
#
# So this benchmark is not expected to show the splitter winning. It is here to put a number
# on what it costs when a project cannot be split -- the fallback path is not free, and
# "falls back safely" is a claim worth measuring rather than asserting. The fallback count is
# reported on every row rather than treated as an abort, because here it is the finding.
#
# A subset of the test targets, not all 24: the full suite takes hours and the shape of the
# answer does not change with more of it. The subset spans four directories so that a header
# edit invalidates more than one.
#
# Usage: ./benchmark-geometry-tests.sh [runs]        (default 1)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-1}"
PLAIN=/tmp/geo-tests-plain
SPLIT=/tmp/geo-tests-split
BOOST="$REPO/example/boost-to-split"
SPLIT_LOG=/tmp/geo-tests-split.log

# Six targets across four directories.
TARGETS="boost_geometry_util_range
boost_geometry_algorithms_area
boost_geometry_algorithms_area_multi
boost_geometry_algorithms_convex_hull
boost_geometry_algorithms_convex_hull_multi
boost_geometry_algorithms_approximately_equals"

# One test source, and a Geometry header every one of the targets includes.
SOURCE="$BOOST/libs/geometry/test/algorithms/area/area.cpp"
HEADER="$BOOST/libs/geometry/include/boost/geometry/algorithms/area.hpp"

# A *different* header for the body edit, and the reason is the whole point of that row.
#
# area.hpp holds nothing but templates. A member of a class template is kept in the preamble
# and no piece is emitted for it (TODO/27), so editing its body could never recompile "one
# piece": it could only re-split every unit that includes it. Timing that and calling it the
# body-edit row measures the splitter's worst case and reports it as its design case.
#
# side_info is an ordinary class and collinear() an ordinary inline member, so the splitter
# emits and compiles a real piece for it -- side_info.hpp_6_collinear.o exists in every unit
# below that includes the header. That is the shape this row is supposed to be about.
#
# Boost.Geometry has very few of these: it is header-only and almost entirely templates, and
# of the ~570 Geometry headers one of these units includes, side_info.hpp is among the only
# ones contributing a compiled piece at all. boost_geometry_util_range does not include it
# and so does not rebuild on this row -- equally, on both sides, so the ratio stands.
BODY_HEADER="$BOOST/libs/geometry/include/boost/geometry/strategies/side_info.hpp"
BODY_BACKUP="$(mktemp)"

# The body edit rewrites a tracked source file, so restore it whatever happens.
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP"' EXIT

# Change the body of side_info::collinear() without changing what it does. Each call inserts a
# different marker, so successive edits are genuinely different text.
patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "    inline bool collinear() const\n    {\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "        (void)%s;  // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PROBE
}

ms() { date +%s%3N; }
human() { awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; }

# grep -c prints 0 and *exits 1* when it finds nothing, so a naive `|| echo 0` prints twice.
fallbacks() {
    [ -r "$SPLIT_LOG" ] || { echo 0; return; }
    grep -c 'falling back' "$SPLIT_LOG" || true
}

build() {  # build <dir> -> elapsed ms
    local dir="$1" start end log=/dev/null
    [ "$dir" = "$SPLIT" ] && log="$SPLIT_LOG"
    start=$(ms)
    # -j8, not -j32: a Geometry test unit is a large template instantiation and the splitter
    # holds a libclang AST of the same unit alongside the compile. Thirty-two at once
    # exhausted 122 GiB, and the OOM killer's victims looked exactly like splitter defects.
    ( cd "$dir" && CPP_SPLITTER_VERBOSE=1 tipi run ninja -j8 $TARGETS ) > "$log" 2>&1 \
        || { echo "BUILD FAILED in $dir" >&2; exit 1; }
    end=$(ms)
    if [ "$dir" = "$SPLIT" ] && grep -q '^Killed' "$SPLIT_LOG"; then
        echo "OOM kills in $dir -- this is not a measurement" >&2
        exit 1
    fi
    echo $((end - start))
}

configure() {  # configure <dir> [launcher...]
    local dir="$1"; shift
    rm -rf "$dir"
    tipi run cmake -GNinja -S "$BOOST" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        -DBOOST_INCLUDE_LIBRARIES=geometry \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=ON \
        "$@" >/dev/null 2>&1
}

LINKER="${CPP_SPLITTER_LINKER:-ld}"
export CPP_SPLITTER_LINKER="$LINKER"

echo "==> building cpp-splitter"
echo "==> relocatable linker: $LINKER"
echo "==> targets: $(echo "$TARGETS" | wc -w)"
tipi run cmake --build "$REPO/build" -j32 >/dev/null

printf '\n%-14s %10s %10s %10s %10s\n' scenario plain split ratio fallbacks
printf '%-14s %10s %10s %10s %10s\n' -------------- ---------- ---------- ---------- ----------

report() {  # report <label> <plain-ms> <split-ms>
    printf '%-14s %10s %10s %9sx %10s\n' "$1" "$(human "$2")" "$(human "$3")" \
        "$(ratio_of "$2" "$3")" "$(fallbacks)"
}

for run in $(seq 1 "$RUNS"); do
    [ "$RUNS" -gt 1 ] && echo "-- run $run --"

    configure "$PLAIN"
    configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"

    report "full"       "$(build "$PLAIN")"  "$(build "$SPLIT")"

    # Twice each. One is not always enough: this project discovers its tests with a CMake
    # GLOB, so the first build after any edit re-runs the glob check and relinks, and a
    # "no-op" measured straight afterwards is really that second pass. The splitter does not
    # run in either -- it settles in one -- but the row would blame it.
    build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
    build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
    report "no-op"      "$(build "$PLAIN")"  "$(build "$SPLIT")"

    touch "$SOURCE"; p=$(build "$PLAIN")
    touch "$SOURCE"; s=$(build "$SPLIT")
    report "one source" "$p" "$s"

    touch "$HEADER"; p=$(build "$PLAIN")
    touch "$HEADER"; s=$(build "$SPLIT")
    report "one header" "$p" "$s"

    cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body "$((run * 2))"
    p=$(build "$PLAIN")
    cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body "$((run * 2 + 1))"
    s=$(build "$SPLIT")
    report "one body" "$p" "$s"
    cp "$BODY_BACKUP" "$BODY_HEADER"
done

echo
echo "==> what fell back, and why"
if [ "$(fallbacks)" -eq 0 ]; then
    echo "    nothing"
else
    grep -m1 -B6 'falling back' "$SPLIT_LOG" \
        | grep -E 'error:|multiple definition|undefined reference' | head -3 | sed 's/^/    /'
fi

echo
echo "==> artefacts"
for name in plain split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || echo "$SPLIT")
    tree=$(du -sh "$dir" 2>/dev/null | cut -f1)
    printf '    %-6s build tree %s\n' "$name" "${tree:-?}"
done
pieces=$(find "$SPLIT" -name '*.cpp' -path '*.split*' 2>/dev/null | wc -l)
echo "    split pieces generated: $pieces"
echo "    relocatable linker:     $LINKER"
