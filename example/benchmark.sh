#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SPLITTER="$PROJECT_DIR/cpp-splitter"
SOURCE="$SCRIPT_DIR/data_processing.cpp"
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O0 -I$SCRIPT_DIR"

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
    make -C "$PROJECT_DIR" -j$(nproc) > /dev/null 2>&1
fi

lines=$(wc -l < "$SOURCE")
funcs=$("$SPLITTER" "$SOURCE" /tmp/bench_count 2>/dev/null | grep -c "^  \[" || true)
rm -rf /tmp/bench_count
echo "Source stats: $lines lines, $funcs functions"
echo "Hardware threads: $(nproc)"
echo ""

time_ms() {
    local start=$(date +%s%N)
    eval "$@" > /dev/null 2>/dev/null
    local end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

echo "--- Step 1: Monolithic full build ---"
t_mono=$(time_ms "$CXX $CXXFLAGS -o /tmp/bench_mono '$SOURCE'")
echo "  Time: ${t_mono}ms"
rm -f /tmp/bench_mono
echo ""

echo "--- Step 2: Split + parallel compile (cold) ---"
rm -rf /tmp/bench_split_out /tmp/bench_split_bin
t_split=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
echo "  Time: ${t_split}ms"
echo ""

echo "--- Step 3: Incremental rebuild (no changes) ---"
t_noop=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
echo "  Time: ${t_noop}ms"
echo ""

echo "--- Step 4: Incremental rebuild (1 file touched) ---"
first_split=$(ls /tmp/bench_split_out/*.cpp 2>/dev/null | grep -v preamble | head -1)
if [ -n "$first_split" ]; then
    sleep 0.1
    touch "$first_split"
    t_one=$(time_ms "'$SPLITTER' '$SOURCE' /tmp/bench_split_out --compile -o /tmp/bench_split_bin --cxx '$CXX' -- $CXXFLAGS")
    echo "  Time: ${t_one}ms"
else
    t_one=0
    echo "  (skipped - no split files found)"
fi
rm -rf /tmp/bench_split_out /tmp/bench_split_bin
echo ""

echo "--- Step 5: Monolithic rebuild after change ---"
echo "(Simulates touching one function in original source)"
echo "  Always rebuilds everything: ${t_mono}ms"
echo ""

echo "============================================"
echo "  Results Summary"
echo "============================================"
echo ""
echo "  Full build (monolithic):       ${t_mono}ms"
echo "  Full build (split+parallel):   ${t_split}ms"
echo "  Incremental (no changes):      ${t_noop}ms"
if [ "$t_one" -gt 0 ]; then
    echo "  Incremental (1 function):      ${t_one}ms"
fi
echo ""
echo "  --- Incremental vs Monolithic rebuild ---"
if [ "$t_one" -gt 0 ] && [ "$t_mono" -gt 0 ]; then
    savings=$((100 - t_one * 100 / t_mono))
    speedup_x=$(echo "scale=1; $t_mono / $t_one" | bc 2>/dev/null || echo "N/A")
    echo "  Editing 1 function: ${t_one}ms vs ${t_mono}ms (${speedup_x}x faster, ${savings}% saved)"
fi
if [ "$t_noop" -gt 0 ] && [ "$t_mono" -gt 0 ]; then
    savings_noop=$((100 - t_noop * 100 / t_mono))
    echo "  No-change rebuild:  ${t_noop}ms vs ${t_mono}ms (${savings_noop}% saved)"
fi
echo ""
echo "============================================"
