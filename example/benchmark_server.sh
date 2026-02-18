#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SPLITTER="$PROJECT_DIR/cpp-splitter"
SOURCE_DP="$SCRIPT_DIR/data_processing.cpp"
SOURCE_SP="$SCRIPT_DIR/spirit_example.cpp"
CXX="${CXX:-g++}"
BOOST_INC="${BOOST_INC:-/nix/store/hp8fsx1sg3dadia1i092188ll9d216sg-boost-1.87.0-dev/include}"

echo "============================================"
echo "  cpp-splitter Server Mode Benchmark"
echo "============================================"
echo ""
echo "Compiler: $CXX"
echo "Hardware threads: $(nproc)"
echo ""

if [ ! -f "$SPLITTER" ]; then
    echo "Building cpp-splitter..."
    make -C "$PROJECT_DIR" -j$(nproc) > /dev/null 2>&1
fi

time_ms() {
    local start=$(date +%s%N)
    eval "$@" > /dev/null 2>/dev/null
    local end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

cleanup_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -f /tmp/cpp-splitter-*.sock
}
trap cleanup_server EXIT

run_benchmark() {
    local label="$1"
    local source="$2"
    local cxxflags="$3"

    local lines=$(wc -l < "$source")
    local funcs=$("$SPLITTER" "$source" /tmp/bench_count 2>/dev/null | grep -c "^  \[" || true)
    rm -rf /tmp/bench_count

    echo "============================================"
    echo "  $label"
    echo "  $source ($lines lines, $funcs functions)"
    echo "============================================"
    echo ""

    echo "--- Monolithic full build ---"
    rm -f /tmp/bench_mono
    t_mono=$(time_ms "$CXX $cxxflags -o /tmp/bench_mono '$source'")
    echo "  Time: ${t_mono}ms"
    rm -f /tmp/bench_mono
    echo ""

    echo "--- Split only WITHOUT server ---"
    rm -rf /tmp/bench_nosrv
    CPP_SPLITTER_NO_SERVER=1 t_split_nosrv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_nosrv -- $cxxflags")
    echo "  Time: ${t_split_nosrv}ms"
    rm -rf /tmp/bench_nosrv
    echo ""

    echo "--- Starting server ---"
    cleanup_server 2>/dev/null || true
    "$SPLITTER" --server &>/dev/null &
    SERVER_PID=$!
    sleep 2
    echo "  Server PID: $SERVER_PID"
    echo ""

    echo "--- Split via server (cold - first parse) ---"
    rm -rf /tmp/bench_srv
    t_split_cold=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv -- $cxxflags")
    echo "  Time: ${t_split_cold}ms"
    echo ""

    echo "--- Split via server (warm - cached TU, 3 runs) ---"
    t_split_warm_total=0
    for i in 1 2 3; do
        rm -rf /tmp/bench_srv
        t=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv -- $cxxflags")
        echo "  Run $i: ${t}ms"
        t_split_warm_total=$((t_split_warm_total + t))
    done
    t_split_warm=$((t_split_warm_total / 3))
    echo "  Average: ${t_split_warm}ms"
    echo ""

    echo "--- Split via server (reparse after source touch) ---"
    touch "$source"
    sleep 0.1
    rm -rf /tmp/bench_srv
    t_split_reparse=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv -- $cxxflags")
    echo "  Time: ${t_split_reparse}ms"
    echo ""

    echo "--- Full build WITHOUT server (cold) ---"
    rm -rf /tmp/bench_nosrv_compile
    CPP_SPLITTER_NO_SERVER=1 t_full_nosrv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_nosrv_compile --compile -o /tmp/bench_nosrv_bin --cxx '$CXX' -- $cxxflags")
    echo "  Time: ${t_full_nosrv}ms"
    echo ""

    echo "--- 1-function rebuild WITHOUT server ---"
    first=$(ls /tmp/bench_nosrv_compile/*.cpp 2>/dev/null | grep -v preamble | head -1)
    if [ -n "$first" ]; then
        sleep 0.1; touch "$first"
        CPP_SPLITTER_NO_SERVER=1 t_inc_nosrv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_nosrv_compile --compile -o /tmp/bench_nosrv_bin --cxx '$CXX' -- $cxxflags")
        echo "  Time: ${t_inc_nosrv}ms"
    else
        t_inc_nosrv=0
        echo "  (skipped)"
    fi
    echo ""

    echo "--- No-change rebuild WITHOUT server ---"
    CPP_SPLITTER_NO_SERVER=1 t_noop_nosrv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_nosrv_compile --compile -o /tmp/bench_nosrv_bin --cxx '$CXX' -- $cxxflags")
    echo "  Time: ${t_noop_nosrv}ms"
    rm -rf /tmp/bench_nosrv_compile /tmp/bench_nosrv_bin
    echo ""

    echo "--- Full build WITH server (warm TU cache) ---"
    rm -rf /tmp/bench_srv_compile
    t_full_srv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv_compile --compile -o /tmp/bench_srv_bin --cxx '$CXX' -- $cxxflags")
    echo "  Time: ${t_full_srv}ms"
    echo ""

    echo "--- 1-function rebuild WITH server ---"
    first=$(ls /tmp/bench_srv_compile/*.cpp 2>/dev/null | grep -v preamble | head -1)
    if [ -n "$first" ]; then
        sleep 0.1; touch "$first"
        t_inc_srv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv_compile --compile -o /tmp/bench_srv_bin --cxx '$CXX' -- $cxxflags")
        echo "  Time: ${t_inc_srv}ms"
    else
        t_inc_srv=0
        echo "  (skipped)"
    fi
    echo ""

    echo "--- No-change rebuild WITH server ---"
    t_noop_srv=$(time_ms "'$SPLITTER' '$source' /tmp/bench_srv_compile --compile -o /tmp/bench_srv_bin --cxx '$CXX' -- $cxxflags")
    echo "  Time: ${t_noop_srv}ms"
    rm -rf /tmp/bench_srv_compile /tmp/bench_srv_bin
    echo ""

    cleanup_server 2>/dev/null || true

    echo "============================================"
    echo "  Results: $label"
    echo "============================================"
    echo ""
    echo "  Splitting only:"
    echo "    Local (no server):       ${t_split_nosrv}ms"
    echo "    Server cold:             ${t_split_cold}ms"
    echo "    Server warm (avg):       ${t_split_warm}ms"
    echo "    Server reparse:          ${t_split_reparse}ms"
    if [ "$t_split_nosrv" -gt 0 ] && [ "$t_split_warm" -gt 0 ]; then
        speedup=$((t_split_nosrv / t_split_warm))
        echo "    Warm speedup:            ${speedup}x"
    fi
    echo ""
    echo "  Full build cycle:"
    echo "    Monolithic:              ${t_mono}ms"
    echo "    Cold (no server):        ${t_full_nosrv}ms"
    echo "    Cold (server warm TU):   ${t_full_srv}ms"
    echo ""
    echo "  Incremental (1 function):"
    echo "    Without server:          ${t_inc_nosrv}ms"
    echo "    With server:             ${t_inc_srv}ms"
    if [ "$t_mono" -gt 0 ] && [ "$t_inc_srv" -gt 0 ]; then
        speedup_x=$((t_mono * 10 / t_inc_srv))
        speedup_int=$((speedup_x / 10))
        speedup_frac=$((speedup_x % 10))
        echo "    vs monolithic:           ${speedup_int}.${speedup_frac}x faster"
    fi
    echo ""
    echo "  No-change rebuild:"
    echo "    Without server:          ${t_noop_nosrv}ms"
    echo "    With server:             ${t_noop_srv}ms"
    echo ""
}

run_benchmark "Data Processing (std headers)" "$SOURCE_DP" "-std=c++17 -O0 -I$SCRIPT_DIR"

if [ -d "$BOOST_INC/boost/spirit" ]; then
    echo ""
    echo ""
    run_benchmark "Boost.Spirit (template-heavy)" "$SOURCE_SP" "-std=c++17 -O0 -I$SCRIPT_DIR -I$BOOST_INC"
else
    echo ""
    echo "(Skipping Boost.Spirit benchmark — headers not found at $BOOST_INC)"
    echo "Set BOOST_INC=/path/to/boost/include to enable."
fi

echo ""
echo "============================================"
echo "  Benchmark complete"
echo "============================================"
