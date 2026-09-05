#!/usr/bin/env bash
#
# Split a wider slice of Boost than the filesystem example, including its test suites.
#
# The filesystem example is twelve translation units of ordinary library code. Boost's test
# suites are a different shape -- hundreds of small translation units, each pulling in heavy
# headers -- and Spirit is different again: header-only, and about as template-dense as C++
# gets. Geometry is a third shape: header-only like Spirit, but where Spirit's weight is in
# expression templates, Geometry's is in tag dispatch over a large concept hierarchy, and its
# tests are real programs rather than compile checks. Each finds things the others cannot.
#
# Every library's tests are built twice, once through the splitter and once without, and the
# two are compared. A test that fails to build without the splitter is not the splitter's
# fault and is not counted against it; only the difference matters.
#
# Usage: ./test-boost-libraries.sh [libraries]
#        ./test-boost-libraries.sh 'filesystem;spirit;system;core'
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOOST="$REPO/example/boost-to-split"
LIBS="${1:-filesystem;spirit;system;core;geometry;smart_ptr;assert}"
SPLIT=/tmp/boost-libs-split
PLAIN=/tmp/boost-libs-plain

# `tipi run` re-splits its arguments through a shell, so the CMake list separator has to
# survive that as well as this script's own quoting.
LIBS_ESCAPED="${LIBS//;/\\;}"

echo "==> libraries: $LIBS"
echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j32 >/dev/null || exit 1

configure() {   # configure <dir> [extra cmake args...]
    rm -rf "$1"
    local dir="$1"; shift
    tipi run cmake -GNinja -S "$BOOST" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        "-DBOOST_INCLUDE_LIBRARIES=$LIBS_ESCAPED" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=ON \
        "$@" >/dev/null 2>&1
}

# -k 0 so one failure does not hide the rest, which is the whole point of the comparison.
build_tests() {  # build_tests <dir> <logfile> -> seconds
    local start end
    start=$(date +%s%3N)
    ( cd "$1" && CPP_SPLITTER_VERBOSE=1 tipi run ninja -j32 -k 0 tests ) > "$2" 2>&1
    end=$(date +%s%3N)
    awk -v v="$((end - start))" 'BEGIN { printf "%.1f", v/1000 }'
}

echo "==> configuring"
configure "$PLAIN"
configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"

echo "==> building test suites without the splitter"
t_plain=$(build_tests "$PLAIN" /tmp/boost-libs-plain.log)
echo "==> building test suites through the splitter"
t_split=$(build_tests "$SPLIT" /tmp/boost-libs-split.log)

count() { grep -c "$1" "$2" 2>/dev/null || true; }

plain_objs=$(count '^\[[0-9]*/[0-9]*\] Building' /tmp/boost-libs-plain.log)
split_objs=$(count '^\[[0-9]*/[0-9]*\] Building' /tmp/boost-libs-split.log)
plain_fail=$(count '^FAILED:' /tmp/boost-libs-plain.log)
split_fail=$(count '^FAILED:' /tmp/boost-libs-split.log)

printf '\n%-34s %10s %10s\n' '' plain split
printf '%-34s %10s %10s\n' '----------------------------------' ---------- ----------
printf '%-34s %10s %10s\n' 'wall time (s)'          "$t_plain"    "$t_split"
printf '%-34s %10s %10s\n' 'objects built'          "$plain_objs" "$split_objs"
printf '%-34s %10s %10s\n' 'failed edges'           "$plain_fail" "$split_fail"
printf '%-34s %10s %10s\n' 'translation units split' '-'          "$(count '^\[cpp-splitter\] done:' /tmp/boost-libs-split.log)"
printf '%-34s %10s %10s\n' 'fallbacks to plain'      '-'          "$(count 'falling back' /tmp/boost-libs-split.log)"

echo
if [ "$split_fail" -gt "$plain_fail" ]; then
    echo "==> $((split_fail - plain_fail)) target(s) fail only with the splitter:"
    comm -13 <(grep '^FAILED:' /tmp/boost-libs-plain.log | sort -u) \
             <(grep '^FAILED:' /tmp/boost-libs-split.log | sort -u) | head -10 | sed 's/^/    /'
else
    echo "==> no target fails that would not also fail without the splitter"
fi

# Spirit has no CMake test suite, being header-only, so exercise it directly. It is the
# densest template code in Boost and the slowest single translation unit here by far.
echo
echo "==> Spirit consumer (no CMake tests exist for a header-only library)"
INC=""; for d in "$BOOST"/libs/*/include; do INC="$INC -I$d"; done
CXX=$(tipi run cmake -LA -N "$SPLIT" 2>/dev/null | sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p')
for mode in plain split; do
    rm -rf /tmp/spirit_$mode.o /tmp/spirit_$mode.o.split
    start=$(date +%s%3N)
    if [ "$mode" = plain ]; then
        tipi run "$CXX" -std=c++17 $INC -I"$REPO/example" \
            -c -o /tmp/spirit_$mode.o "$REPO/example/spirit_example.cpp" >/dev/null 2>&1
    else
        CPP_SPLITTER_VERBOSE=1 "$REPO/build/cpp-splitter" "$CXX" -std=c++17 $INC \
            -I"$REPO/example" -c -o /tmp/spirit_$mode.o \
            "$REPO/example/spirit_example.cpp" > /tmp/spirit_split.log 2>&1
    fi
    end=$(date +%s%3N)
    printf '    %-6s %ss' "$mode" "$(awk -v v="$((end - start))" 'BEGIN{printf "%.1f", v/1000}')"
    if [ "$mode" = split ]; then
        printf '  pieces %s  fallback %s' \
            "$(find /tmp/spirit_split.o.split -name '*.cpp' 2>/dev/null | wc -l)" \
            "$(count 'falling back' /tmp/spirit_split.log)"
    fi
    echo
done
