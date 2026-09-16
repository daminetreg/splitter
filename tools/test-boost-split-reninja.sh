#!/usr/bin/env bash
#
# Split Boost.Filesystem with cpp-splitter, driven by reninja instead of ninja.
#
# reninja (https://github.com/buildbuddy-io/reninja) is a ninja-compatible build system that
# tracks content rather than modification time. That distinction matters here: the splitter
# rewrites a piece only when its text actually differs, so most of what it regenerates is
# byte-for-byte identical to what was there before, and a timestamp-based build system
# rebuilds everything downstream of it anyway. Two workarounds in the launcher exist only to
# speak the timestamp vocabulary -- touching the object when the relocatable link is skipped,
# and caching the dependency file so a build that compiles nothing still reports the
# dependencies it knows about.
#
# Usage: ./test-boost-split-reninja.sh [build-dir]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-/tmp/boost-split-reninja}"
RENINJA="$REPO/tools/bin/reninja"
RENINJA_URL=https://github.com/buildbuddy-io/reninja/releases/download/v0.1.4/reninja-linux-amd64

# --- 1. reninja ------------------------------------------------------------------------
if [ ! -x "$RENINJA" ]; then
    echo "==> installing reninja to $RENINJA"
    mkdir -p "$(dirname "$RENINJA")"
    curl -fsSL -o "$RENINJA" "$RENINJA_URL"
    chmod +x "$RENINJA"
fi
echo "==> reninja $("$RENINJA" --version)"

# --- 2. the splitter -------------------------------------------------------------------
echo "==> building cpp-splitter"
tipi run cmake -GNinja -S "$REPO" -B "$REPO/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" >/dev/null
tipi run cmake --build "$REPO/build" -j32 >/dev/null

# --- 3. configure Boost.Filesystem to build through the splitter, driven by reninja -----
echo "==> configuring $OUT"
rm -rf "$OUT"
tipi run cmake -GNinja -S "$REPO/example/boost-to-split" -B "$OUT" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
    -DCMAKE_MAKE_PROGRAM="$RENINJA" \
    -DBOOST_INCLUDE_LIBRARIES=filesystem \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter" >/dev/null

cd "$OUT"


# --- 4. build --------------------------------------------------------------------------
echo "==> building (this writes a few GB of split output; check df first)"
CPP_SPLITTER_VERBOSE=1 "$RENINJA" -j8 2>&1 | tee split-build.log | tail -1

split_count() { grep -c '^\[cpp-splitter\] done:' split-build.log || true; }
echo "    translation units linked from split objects: $(split_count) / 12"
echo "    fallbacks: $(grep -c 'falling back' split-build.log || true)"

# --- 5. does it settle? ----------------------------------------------------------------
echo "==> settle check (a second build should do nothing)"
for i in 1 2; do
    n=$("$RENINJA" -j8 2>&1 | grep -cE '^\[[0-9]+/' || true)
    echo "    run $i rebuilt $n target(s)"
done

# --- 6. does an edit to a split header propagate? --------------------------------------
HEADER="$REPO/example/boost-to-split/libs/filesystem/include/boost/filesystem/path.hpp"
echo "==> touching $(basename "$HEADER")"
touch "$HEADER"
"$RENINJA" -j8 2>&1 | grep -E '^\[[0-9]+/' | sed 's|.*dir/||;s|Linking.*|link|' | tr '\n' ' '
echo

# --- 7. and a content-identical rewrite? -----------------------------------------------
# This is the case a content-tracking build system should handle better than a timestamp
# one: the file's contents are unchanged, only its modification time moved.
echo "==> touching it again with identical content"
touch "$HEADER"
"$RENINJA" -d explain -j8
