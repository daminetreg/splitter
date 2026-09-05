#!/usr/bin/env bash
#
# Compare building a Boost.Spirit consumer through cpp-splitter against building it normally.
#
# This is benchmark-boost-split.sh's five scenarios asked of the other kind of code. Boost's
# libraries are ordinary C++ compiled into a static library; Spirit is template metaprogramming
# where a single translation unit costs seconds and one shared header reaches into most of
# Boost. Splitting is expensive in proportion to how much a unit costs to parse, and it saves
# in proportion to how much of that cost an edit repeats -- so Spirit is where both sides of
# the trade are largest, and the ratios do not carry over from the filesystem benchmark.
#
#   full         one function per object file is strictly more work than one object per
#                source, and on Spirit each of those objects re-instantiates the grammar. This
#                is the cost, and it is the worst number in the table.
#   no-op        a settled tree should do nothing. Anything else is a bug, not a measurement.
#   one source   only the pieces whose text changed should be recompiled, rather than the
#                whole translation unit.
#   one header   the shared header's timestamp changes but its content does not. The build
#                system re-runs the splitter on every unit that includes it, and the split
#                cache should recognise the inputs and reuse the previous split (TODO 14).
#   one body     the case the whole design is aimed at: change the body of one inline function
#                in the shared header. Without splitting, every unit re-instantiates every
#                grammar it has. With it, only the piece holding that function should need
#                recompiling -- but the split itself has to run again, and on Spirit that
#                re-parse is the number that decides whether any of this pays.
#
# Usage: ./benchmark-spirit-split.sh [runs]        (default 1)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-1}"
PLAIN=/tmp/spirit-bench-plain
SPLIT=/tmp/spirit-bench-split
PROJECT="$REPO/example/spirit-bench"
BOOST="$REPO/example/boost-to-split"
SOURCE="$PROJECT/xml.cpp"
HEADER="$PROJECT/bench_common.hpp"
HEADER_BACKUP="$(mktemp)"

# The body edit rewrites a tracked source file, so restore it whatever happens.
cp "$HEADER" "$HEADER_BACKUP"
trap 'cp "$HEADER_BACKUP" "$HEADER"; rm -f "$HEADER_BACKUP"' EXIT

# Change the body of bench_weight() without changing what it does. Each call inserts a
# different marker, so successive edits are genuinely different text.
patch_body() {
    python3 - "$HEADER" "$1" <<'PY'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "inline unsigned int bench_weight(unsigned int n) {\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
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
    tipi run cmake -GNinja -S "$PROJECT" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        -DBOOST_ROOT_DIR="$BOOST" \
        "$@" >/dev/null 2>&1
}

LINKER="${CPP_SPLITTER_LINKER:-ld}"
export CPP_SPLITTER_LINKER="$LINKER"

echo "==> building cpp-splitter"
echo "==> relocatable linker: $LINKER"
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
    cp "$HEADER_BACKUP" "$HEADER"; patch_body "$((run * 2))"
    p=$(build "$PLAIN")
    cp "$HEADER_BACKUP" "$HEADER"; patch_body "$((run * 2 + 1))"
    s=$(build "$SPLIT")
    report "one body" "$p" "$s"
    cp "$HEADER_BACKUP" "$HEADER"
done

# The two programs must agree, or the benchmark is timing a different program.
echo
echo "==> output"
build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
if diff <("$PLAIN/spirit_bench") <("$SPLIT/spirit_bench") >/dev/null 2>&1; then
    echo "    split and plain print the same thing"
else
    echo "    MISMATCH: the split program does not agree with the plain one"
fi

echo
echo "==> artefacts"
for name in plain split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || echo "$SPLIT")
    bin=$(du -h "$dir/spirit_bench" 2>/dev/null | cut -f1)
    tree=$(du -sh "$dir" 2>/dev/null | cut -f1)
    printf '    %-6s binary %-8s build tree %s\n' "$name" "${bin:-?}" "${tree:-?}"
done
pieces=$(find "$SPLIT" -name '*.cpp' -path '*.split*' 2>/dev/null | wc -l)
echo "    split pieces generated: $pieces"
echo "    relocatable linker:     $LINKER"
