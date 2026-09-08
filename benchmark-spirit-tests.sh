#!/usr/bin/env bash
#
# Compare building Boost.Spirit's own test programs through cpp-splitter against building them
# normally.
#
# The other Spirit benchmark measures a *consumer* -- code written against the library. This
# one measures the library's own tests, which is a different question and on Geometry gave the
# opposite answer: a consumer's body-edit row loses while the test suite's wins, because six
# real programs give the saving more to work with than four translation units and a driver.
#
# Spirit ships no CMakeLists for its tests. It is header-only and its tests are driven by a
# Boost.Build Jamfile, so the superproject's BUILD_TESTING produces no Spirit targets at all --
# which is why the four-library harness compiles a consumer for Spirit and calls it that.
# example/spirit-tests/ builds the real sources from libs/spirit/test as the programs the
# Jamfile would build; only the driver differs. All 277 of them -- the `run` and `compile`
# targets its Jamfiles declare, read out of those Jamfiles rather than listed by hand so this
# cannot drift from the suite it claims to be. The four `compile-fail` sources are the only
# thing left out, and they are supposed to fail.
#
#   full         one object per function, each re-instantiating the grammar its function
#                needs. This is the cost, and it is the worst number in the table.
#   no-op        a settled tree should do nothing. Anything else is a bug, not a measurement.
#   one source   touch one test .cpp.
#   one header   touch a Spirit header every target includes -- timestamp only, content
#                identical, so the split cache should recognise the inputs (TODO 14).
#   one body     change the body of one ordinary inline function in a header the splitter
#                emits a piece for. See BODY_HEADER below for why that qualification matters.
#
# Fallbacks are reported on every row rather than aborting the run, as in the Geometry test
# benchmark: on a library's own test suite a fallback is a finding, and a row where the
# splitter gave up is not comparable with one where it did not.
#
# Usage: ./benchmark-spirit-tests.sh [runs]        (default 1)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-1}"
PLAIN=/tmp/spirit-tests-plain
SPLIT=/tmp/spirit-tests-split
SPLIT_LOG=/tmp/spirit-tests-split.log
PROJECT="$REPO/example/spirit-tests"
BOOST="$REPO/example/boost-to-split"

# Everything the project defines. Empty rather than a list: `ninja` with no target builds all,
# and naming 277 targets on the command line adds nothing but a way to fall out of sync.
TARGETS=""

# One test source, and a Spirit header nearly every target includes.
SOURCE="$BOOST/libs/spirit/test/qi/char1.cpp"
HEADER="$BOOST/libs/spirit/include/boost/spirit/home/support/char_encoding/standard.hpp"

# A different header for the body edit, and the reason is the point of that row.
#
# Nearly all of Spirit is templates, and the splitter keeps a template in the preamble without
# emitting a piece for it (TODO 27) -- so editing one could never recompile "one piece", only
# force every unit that includes it to be re-split. Timing that and calling it the body-edit
# row measures the worst case and reports it as the design case, which is the mistake the
# Geometry benchmark made until 8 September.
#
# utf8_put_encode() is an ordinary non-template free function in Spirit's own header, and the
# splitter emits and compiles a real piece for it. Of the handful of Spirit headers
# contributing any compiled piece at all it is the one reaching the most units:
# char_encoding/standard.hpp's members are all kept as "not emitted by this translation unit",
# and char_encoding/ascii.hpp's reach fewer.
BODY_HEADER="$BOOST/libs/spirit/include/boost/spirit/home/support/utf8.hpp"
BODY_BACKUP="$(mktemp)"

# The body edit rewrites a tracked source file, so restore it whatever happens.
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP"' EXIT

# Change the body of utf8_put_encode() without changing what it does. Each call inserts a
# different marker, so successive edits are genuinely different text.
patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "    inline void utf8_put_encode(utf8_string& out, ucs4_char x)\n    {\n"
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
    # -j8, not -j32: a Spirit test unit is a very large template instantiation and the splitter
    # holds a libclang AST of the same unit alongside the compile. The Geometry suite exhausted
    # 122 GiB at -j32 and the OOM killer's victims looked exactly like splitter defects. With
    # 277 units in flight that headroom matters more here than it did with six.
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
echo "==> targets: every test the Jamfiles declare"
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

    # Twice each, because one settle is not always enough: the first build after any edit can
    # carry a generator's own re-check, and a "no-op" measured straight afterwards is really
    # that second pass with the splitter blamed for it.
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
echo "==> how many units re-sliced a body instead of re-parsing (TODO 28)"
echo "    $(grep -c 're-sliced its piece' "$SPLIT_LOG" || true)"

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
