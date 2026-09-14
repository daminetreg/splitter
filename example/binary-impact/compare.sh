#!/usr/bin/env bash
# Builds the use_mylib project five ways and compares what comes out. TODO/46.
#
#   plain              clang++ -O2
#   plain-lto          clang++ -O2 -flto=thin
#   split              cpp-splitter as the compiler launcher, pieces combined with `ld -r`
#   split-lto-relink   -flto=thin; pieces combined with `ld.lld -r`, which runs LTO over the
#                      pieces of a unit and writes a native object -- the final link sees no
#                      bitcode
#   split-lto-final    -flto=thin; pieces merged with llvm-link into one bitcode object, so
#                      the final link optimises across the whole program as plain-lto does
#
# Everything is written under results/: one build tree per configuration, the executable and
# the unit's object of each, and the reports:
#   results/report.md                 sizes, program output, and the comparisons below
#   results/<a>-vs-<b>.symbols        `nm` symbol sets: only in a, only in b, binding changes
#   results/<a>-vs-<b>.asm            per-function disassembly diff (asmdiff.py)
#   results/<a>-vs-<b>.diffoscope     diffoscope's text report on the executables
#
# The toolchain is the repository's (environments/monolithic.cmake); the splitter is the one
# built in $REPO/build. diffoscope is optional: set DIFFOSCOPE to the program to run.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SPLITTER="${SPLITTER:-$REPO/build/cpp-splitter}"
TOOLCHAIN="$REPO/environments/monolithic.cmake"
CLANG_BIN="$(dirname "$(grep -m1 CMAKE_CXX_COMPILER "$TOOLCHAIN" | awk '{print $2}')")"
OBJDUMP="${OBJDUMP:-objdump}"
RESULTS="$HERE/results"

[ -x "$SPLITTER" ] || { echo "no splitter at $SPLITTER; build it first" >&2; exit 1; }
rm -rf "$RESULTS"
mkdir -p "$RESULTS"

CONFIGS="plain plain-lto split split-lto-relink split-lto-final"

build() {  # build <config>
    local cfg="$1" dir="$RESULTS/build-$1" lto=OFF launcher="" linker=""
    case "$cfg" in
        plain) ;;
        plain-lto) lto=ON ;;
        split) launcher="$SPLITTER" ;;
        split-lto-relink) lto=ON; launcher="$SPLITTER"; linker="$CLANG_BIN/ld.lld" ;;
        split-lto-final) lto=ON; launcher="$SPLITTER"; linker="$HERE/llvm-link-r.sh" ;;
    esac
    local args=(-GNinja -S "$HERE" -B "$dir" -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DBINARY_IMPACT_LTO=$lto
                -DCMAKE_CXX_FLAGS_RELEASE=-O2)
    [ -n "$launcher" ] && args+=(-DCMAKE_CXX_COMPILER_LAUNCHER="$launcher")
    cmake "${args[@]}" > "$dir.configure.log" 2>&1
    CPP_SPLITTER_VERBOSE=1 CPP_SPLITTER_LINKER="$linker" LLVM_LINK="$CLANG_BIN/llvm-link" \
        cmake --build "$dir" > "$dir.build.log" 2>&1 \
        || { echo "build failed: $cfg, see $dir.build.log" >&2; exit 1; }
    if [ -n "$launcher" ] && grep -q "falling back\|not splitting" "$dir.build.log"; then
        echo "$cfg: the unit was not split, see $dir.build.log" >&2; exit 1
    fi
    mkdir -p "$RESULTS/$cfg"
    cp "$dir/use_mylib" "$RESULTS/$cfg/use_mylib"
    cp "$dir/CMakeFiles/use_mylib.dir/use_mylib.cpp.o" "$RESULTS/$cfg/use_mylib.o"
    "$RESULTS/$cfg/use_mylib" > "$RESULTS/$cfg/output.txt"
}

# llvm-nm, because the objects of the LTO configurations are bitcode, which nm does not read.
symbols() {  # symbols <file> -> "binding name" lines, sorted
    "$CLANG_BIN/llvm-nm" --defined-only -C "$1" | awk '{ $1=""; sub(/^ /, ""); print }' | sort
}

is_bitcode() { [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "4243c0de" ]; }

compare() {  # compare <a> <b>
    local a="$1" b="$2" tag="$1-vs-$2" f
    for what in use_mylib use_mylib.o; do
        {
            echo "== $what: symbols only in $a"
            comm -23 <(symbols "$RESULTS/$a/$what") <(symbols "$RESULTS/$b/$what")
            echo "== $what: symbols only in $b"
            comm -13 <(symbols "$RESULTS/$a/$what") <(symbols "$RESULTS/$b/$what")
        } >> "$RESULTS/$tag.symbols"
    done
    "$HERE/asmdiff.py" "$RESULTS/$a/use_mylib" "$RESULTS/$b/use_mylib" \
        --objdump "$OBJDUMP" --label-a "$a" --label-b "$b" \
        --diff "$RESULTS/$tag.asm" > "$RESULTS/$tag.asm.summary"
    if [ -n "${DIFFOSCOPE:-}" ]; then
        "$DIFFOSCOPE" --text "$RESULTS/$tag.diffoscope" \
            "$RESULTS/$a/use_mylib" "$RESULTS/$b/use_mylib" > /dev/null 2>&1 || true
    fi
}

for cfg in $CONFIGS; do
    echo "==> $cfg"
    build "$cfg"
done

REPORT="$RESULTS/report.md"
{
    echo "# use_mylib: five builds"
    echo
    echo "| configuration | executable | .text | .data | .bss | object | object .text | symbols (exe) | symbols (object) |"
    echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
    for cfg in $CONFIGS; do
        exe="$RESULTS/$cfg/use_mylib"; obj="$RESULTS/$cfg/use_mylib.o"
        read -r t d b _ < <(size "$exe" | tail -1)
        if is_bitcode "$obj"; then ot="bitcode"; else read -r ot _ < <(size "$obj" | tail -1); fi
        printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" "$cfg" "$(stat -c %s "$exe")" \
            "$t" "$d" "$b" "$(stat -c %s "$obj")" "$ot" \
            "$(symbols "$exe" | wc -l)" "$(symbols "$obj" | wc -l)"
    done
    echo
    echo "## Program output"
    echo
    first=""
    for cfg in $CONFIGS; do
        if [ -z "$first" ]; then first="$cfg"; continue; fi
        if cmp -s "$RESULTS/$first/output.txt" "$RESULTS/$cfg/output.txt"; then
            echo "- $cfg: identical to $first"
        else
            echo "- $cfg: DIFFERS from $first"
        fi
    done
    echo
    echo '```'
    cat "$RESULTS/plain/output.txt"
    echo '```'
} > "$REPORT"

PAIRS="plain:split plain:plain-lto plain:split-lto-relink plain:split-lto-final plain-lto:split-lto-relink plain-lto:split-lto-final"
for pair in $PAIRS; do
    a="${pair%%:*}"; b="${pair##*:}"
    echo "==> $a vs $b"
    compare "$a" "$b"
    {
        echo
        echo "## $a vs $b"
        echo
        echo '```'
        cat "$RESULTS/$a-vs-$b.asm.summary"
        echo '```'
        echo
        echo "Symbols (executable, then object):"
        echo
        echo '```'
        cat "$RESULTS/$a-vs-$b.symbols"
        echo '```'
        if [ -s "$RESULTS/$a-vs-$b.diffoscope" ]; then
            echo
            echo "diffoscope: $(wc -l < "$RESULTS/$a-vs-$b.diffoscope") lines, in \`results/$a-vs-$b.diffoscope\`"
        fi
    } >> "$REPORT"
done
echo "==> report: $REPORT"
