#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SPLITTER="$PROJECT_DIR/cpp-splitter"
DEMO_FILE="$SCRIPT_DIR/spirit_example.cpp"
SOURCE="$SCRIPT_DIR/currently_benchmarked.cpp"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O0 -I$SCRIPT_DIR -I/Users/daminetreg/workspace/tipi/hermetic-fetchcontent.release-archive/build/_deps/Boost-install/include"

cp $DEMO_FILE ${SOURCE}

echo "============================================"
echo "  cpp-splitter Benchmark: Data Processing"
echo "============================================"
echo ""
echo "Source:   $SOURCE"
echo "Compiler: $CXX"
echo "Flags:    $CXXFLAGS"
echo ""

if [ ! -f "$SPLITTER" ]; then
    echo "Building cpp-splitter..."
    make -C "$PROJECT_DIR" -j12 > /dev/null 2>&1
fi

lines=$(wc -l < "$SOURCE")
funcs=$("$SPLITTER" "$SOURCE" /tmp/bench_count 2>/dev/null | grep -c "^  \[" || true)
rm -rf /tmp/bench_count
echo "Source stats: $lines lines, $funcs functions"
echo "Hardware threads: 12"
echo ""

time_ms() {
    local start=$(gdate +%s%N)
    eval "$@" 1>&2
    local end=$(gdate +%s%N)
    echo $(( (end - start) / 1000000 ))
}

echo "--- Step 1: Monolithic full build ---"
t_mono=$(time_ms "$CXX $CXXFLAGS -o /tmp/bench_mono '$SOURCE'")
echo "  Time: ${t_mono}ms"
#rm -f /tmp/bench_mono
echo ""

echo "--- Step 2: Split + parallel compile (cold) ---"
#rm -rf /tmp/bench_split_out /tmp/bench_split_bin
t_split=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
echo "  Time: ${t_split}ms"
echo ""

echo "--- Step 3: Incremental rebuild (no changes) ---"
t_noop=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
echo "  Time: ${t_noop}ms"
echo ""

echo "--- Step 4: Incremental rebuild (1 function modified) ---"
sed -i bak 's/Boost.Spirit Qi\/Karma Demo/Modified/' ${SOURCE}
t_one=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
echo "  Time: ${t_one}ms"
#rm -rf /tmp/bench_split_out /tmp/bench_split_bin
echo ""

echo "--- Step 5: Monolithic rebuild after change ---"
echo "(Simulates touching one function in original source)"
echo "  Always rebuilds everything: ${t_mono}ms"
t_mono_rebuild=$(time_ms "$CXX $CXXFLAGS -o /tmp/bench_mono '$SOURCE'")
echo "  Time: ${t_mono_rebuild}ms"
#rm -f /tmp/bench_mono
echo ""

echo "============================================"
echo "  Results Summary"
echo "============================================"
echo ""
echo "  Full build (monolithic):       ${t_mono}ms"
echo "  Full build (split+parallel):   ${t_split}ms"
echo "  Incremental (no changes):      ${t_noop}ms"
if [ "$t_one" -gt 0 ]; then
    echo "  Incremental (1 function change):      ${t_one}ms"
    echo "  Monolitic rebuild (1 function change) :      ${t_mono_rebuild}ms"
fi
echo ""
echo "  --- Incremental vs Monolithic rebuild ---"
if [ "$t_one" -gt 0 ] && [ "$t_mono_rebuild" -gt 0 ]; then
    savings=$((100 - t_one * 100 / t_mono_rebuild))
    speedup_x=$(echo "scale=1; $t_mono_rebuild / $t_one" | bc 2>/dev/null || echo "N/A")
    echo "  Editing 1 function: ${t_one}ms vs ${t_mono_rebuild}ms (${speedup_x}x faster, ${savings}% saved)"
fi
if [ "$t_noop" -gt 0 ] && [ "$t_mono" -gt 0 ]; then
    savings_noop=$((100 - t_noop * 100 / t_mono))
    echo "  No-change rebuild:  ${t_noop}ms vs ${t_mono}ms (${savings_noop}% saved)"
fi
echo ""
echo "============================================"
