#!/usr/bin/env bash
#
# Compare building Boost.Filesystem through cpp-splitter against building it normally.
#
# The two are configured identically apart from CMAKE_CXX_COMPILER_LAUNCHER, so the only
# difference measured is the splitting. Four scenarios, because they answer different
# questions and the splitter is not expected to win all of them:
#
#   full         one function per object file is strictly more work than one object file per
#                source, so a cold build is slower. This is the cost.
#   no-op        a settled tree should do nothing. Anything else is a bug, not a measurement.
#   one source   the case splitting is meant to help: only the pieces whose text changed are
#                recompiled, rather than the whole translation unit.
#   one header   the expensive case, since a widely included header invalidates many units.
#   one body     the case the whole design is aimed at: change the body of a single inline
#                function in a widely included header. Without splitting every translation
#                unit that includes the header recompiles in full. With it, only the piece
#                holding that one function should need recompiling.
#
# Usage: ./benchmark-boost-split.sh [runs]        (default 1)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-1}"
PLAIN=/tmp/bench-plain
SPLIT=/tmp/bench-split
BOOST="$REPO/example/boost-to-split"
SOURCE="$BOOST/libs/filesystem/src/portability.cpp"
HEADER="$BOOST/libs/filesystem/include/boost/filesystem/path.hpp"
BODY_HEADER="$BOOST/libs/filesystem/include/boost/filesystem/operations.hpp"
BODY_BACKUP="$(mktemp)"

# The body edit rewrites a tracked source file, so restore it whatever happens.
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP"' EXIT

# Change the body of is_regular_file(path const&) without changing what it does. Each call
# inserts a different marker, so successive edits are genuinely different text.
patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PY'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "inline bool is_regular_file(path const& p)\n{\n"
src = open(path).read()
src = src.replace(signature, signature + "    (void)%s; // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PY
}

ms() { date +%s%3N; }
human() { awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; }


build() {  # build <dir> -> elapsed ms
    local dir="$1" start end
    start=$(ms)
    ( cd "$dir" && tipi run ninja -j32 >/dev/null 2>&1 ) || { echo "BUILD FAILED in $dir" >&2; exit 1; }
    end=$(ms)
    echo $((end - start))
}

configure() {  # configure <dir> [launcher...]
    local dir="$1"; shift
    rm -rf "$dir"
    tipi run cmake -GNinja -S "$BOOST" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        -DBOOST_INCLUDE_LIBRARIES=filesystem \
        -DBUILD_SHARED_LIBS=OFF \
        "$@" >/dev/null 2>&1
}

echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j32 >/dev/null

printf '\n%-14s %10s %10s %10s\n' scenario plain split ratio
printf '%-14s %10s %10s %10s\n' -------------- ---------- ---------- ----------

report() {  # report <label> <plain-ms> <split-ms>
    printf '%-14s %10s %10s %9sx\n' "$1" "$(human "$2")" "$(human "$3")" "$(ratio_of "$2" "$3")"
}

for run in $(seq 1 "$RUNS"); do
    [ "$RUNS" -gt 1 ] && echo "-- run $run --"

    configure "$PLAIN"
    configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"

    report "full"       "$(build "$PLAIN")"  "$(build "$SPLIT")"

    # Settle both first: a build that always has something to do would make every
    # incremental number below meaningless.
    build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
    report "no-op"      "$(build "$PLAIN")"  "$(build "$SPLIT")"

    touch "$SOURCE"; p=$(build "$PLAIN")
    touch "$SOURCE"; s=$(build "$SPLIT")
    report "one source" "$p" "$s"

    touch "$HEADER"; p=$(build "$PLAIN")
    touch "$HEADER"; s=$(build "$SPLIT")
    report "one header" "$p" "$s"

    # The full build above is already done and settled; only this edit is timed.
    cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body "$((run * 2))"
    p=$(build "$PLAIN")
    cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body "$((run * 2 + 1))"
    s=$(build "$SPLIT")
    report "one body" "$p" "$s"
    cp "$BODY_BACKUP" "$BODY_HEADER"
done

echo
echo "==> artefacts"
for name in plain split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || echo "$SPLIT")
    lib=$(du -h "$dir/stage/lib/libboost_filesystem.a" 2>/dev/null | cut -f1)
    tree=$(du -sh "$dir" 2>/dev/null | cut -f1)
    printf '    %-6s library %-8s build tree %s\n' "$name" "${lib:-?}" "${tree:-?}"
done
pieces=$(find "$SPLIT" -name '*.cpp' -path '*.split*' 2>/dev/null | wc -l)
echo "    split pieces generated: $pieces"
