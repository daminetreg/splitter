#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SPLITTER="$REPO_ROOT/cpp-splitter"
BUILD_DIR="$SCRIPT_DIR/build"

if [ ! -x "$SPLITTER" ]; then
    echo "Error: cpp-splitter not found at $SPLITTER"
    echo "Run 'make' in the repo root first."
    exit 1
fi

echo "=== Starting cpp-splitter server ==="
"$SPLITTER" --server &
SERVER_PID=$!
sleep 0.5
trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo ""
echo "=== Configuring with CMake (cpp-splitter as launcher) ==="
TIPI_CPP_SPLITTER_VERBOSE=on cmake \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$SPLITTER" \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR"

echo ""
echo "=== Building ==="
TIPI_CPP_SPLITTER_VERBOSE=on cmake --build "$BUILD_DIR" -- VERBOSE=1

echo ""
echo "=== Running the built binary ==="
"$BUILD_DIR/sample_app"

echo ""
echo "=== Rebuilding (no changes — should be fast) ==="
TIPI_CPP_SPLITTER_VERBOSE=on cmake --build "$BUILD_DIR" -- VERBOSE=1

echo ""
echo "=== Touching sample.cpp and rebuilding (incremental) ==="
sleep 0.1
touch "$REPO_ROOT/test/sample.cpp"
TIPI_CPP_SPLITTER_VERBOSE=on cmake --build "$BUILD_DIR" -- VERBOSE=1

echo ""
echo "Done."
