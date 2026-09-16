#!/usr/bin/env bash
#
# Compare building a C++20 named-module project through cpp-splitter against building it
# normally, on this machine, with Homebrew's clang. TODO/43 Phase 3.
#
# The project is example/cpp-20-modules/bench: one interface unit, `mathlib`, with 40 exported
# non-inline functions and 4 inline ones, and 30 importers. The rows are the usual five with
# the header rows asked of the module instead:
#
#   full          one implementation unit per body, one piece per importer function: strictly
#                 more compiles than one object per source. The cost.
#   no-op         a settled tree does nothing.
#   one source    touch one importer; timestamp only.
#   one module    touch the interface unit; timestamp only. CMake rescans, the launcher
#                 finds its inputs unchanged, the BMI is not rewritten.
#   one body      the row the design is for: one line added to the body of an exported
#                 non-inline function in the interface unit. Plain: the BMI changes (its ODR
#                 hash), every importer recompiles. Split: one implementation unit
#                 recompiles, the BMI is byte-identical, every importer's launcher finds
#                 nothing to do.
#   one body =    the same edit without a new line: a statement changed in place. The
#                 re-slice renumbers no `#line`, so exactly one piece recompiles.
#   inline body   the control: one line added to an exported inline body. The BMI must
#                 change in both builds and every importer recompile in both.
#   importer body one line added to a function body in one importer: the ordinary split.
#
# Usage: ./benchmark-cpp20-modules.sh [runs] [splitter]
#   default 1 run, splitter build/cmake-re-macos-brew-llvm/cpp-splitter
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNS="${1:-1}"
SPLITTER="${2:-$REPO/build/cmake-re-macos-brew-llvm/cpp-splitter}"
PROJECT="$REPO/example/cpp-20-modules/bench"
PLAIN=/tmp/cpp20-modules-bench-plain
SPLIT=/tmp/cpp20-modules-bench-split
MODULE="$PROJECT/mathlib.cppm"
IMPORTER="$PROJECT/use_1.cpp"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

LLVM="$(brew --prefix llvm)"
CXX="$LLVM/bin/clang++"
SDK="$(xcrun --show-sdk-path)"

MODULE_BACKUP="$(mktemp)"; IMPORTER_BACKUP="$(mktemp)"
cp "$MODULE" "$MODULE_BACKUP"; cp "$IMPORTER" "$IMPORTER_BACKUP"
trap 'cp "$MODULE_BACKUP" "$MODULE"; cp "$IMPORTER_BACKUP" "$IMPORTER"; rm -f "$MODULE_BACKUP" "$IMPORTER_BACKUP"' EXIT

# Add a line to a body without changing what it computes; each call a different marker.
patch_body() {  # patch_body <file> <signature line> <marker>
    python3 - "$1" "$2" "$3" <<'PY'
import sys
path, signature, marker = sys.argv[1], sys.argv[2] + "\n", sys.argv[3]
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved: " + signature
src = src.replace(signature, signature + "    (void)%s; // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PY
}
patch_body_sameline() {  # patch_body_sameline <marker>: change compute_17's return in place
    python3 - "$MODULE" "$1" <<'PY'
import sys, re
path, marker = sys.argv[1], sys.argv[2]
src = open(path).read()
start = src.index("export int compute_17(int n) {")
end = src.index("export int compute_18(int n) {")
block = src[start:end]
old = "return static_cast<int>(acc % 1000) + v.front() + n;"
assert block.count(old) == 1
src = src[:start] + block.replace(old, "return static_cast<int>(acc % 1000) + v.front() + n + 0 * " + marker + ";") + src[end:]
open(path, "w").write(src)
PY
}
patch_inline() {  # the inline body is one line; append a statement inside it
    python3 - "$MODULE" "$1" <<'PY'
import sys
path, marker = sys.argv[1], sys.argv[2]
old = "export inline int fast_1(int v) { return v * 3 + 1; }"
src = open(path).read()
assert src.count(old) == 1
src = src.replace(old, "export inline int fast_1(int v) { (void)%s; return v * 3 + 1; }" % marker, 1)
open(path, "w").write(src)
PY
}

ms() { python3 -c 'import time; print(int(time.time()*1000))'; }
human() { awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; }

PLAIN_LOG=/tmp/cpp20-modules-bench-plain.log
SPLIT_LOG=/tmp/cpp20-modules-bench-split.log

build() {  # build <dir> -> elapsed ms; the log is kept for the counts
    local dir="$1" start end log
    log=$([ "$dir" = "$SPLIT" ] && echo "$SPLIT_LOG" || echo "$PLAIN_LOG")
    start=$(ms)
    ( cd "$dir" && CPP_SPLITTER_VERBOSE=1 ninja -j"$JOBS" ) > "$log" 2>&1 \
        || { echo "BUILD FAILED in $dir" >&2; tail -20 "$log" >&2; exit 1; }
    end=$(ms)
    echo $((end - start))
}
# Compiler invocations a build made. Plain: one per object. Split: what the launcher's log
# says it ran -- the interface, sequential pieces, parallel batches, header pieces.
compiles() {  # compiles <log>
    local log="$1"
    if [ "$log" = "$PLAIN_LOG" ]; then
        grep -c 'Building CXX object' "$log" || true
    else
        awk '/compile \(interface\)/ {n++} /compile \(seq\)/ {n++}
             /compiling [0-9]+ split file\(s\)/ {for (i=1;i<=NF;i++) if ($i=="compiling") n+=$(i+1)}
             /compiling [0-9]+ header dep file\(s\)/ {for (i=1;i<=NF;i++) if ($i=="compiling") n+=$(i+1)}
             END {print n+0}' "$log"
    fi
}
objects() { grep -c 'Building CXX object' "$1" || true; }
fallbacks() { [ -r "$SPLIT_LOG" ] || { echo 0; return; }; grep -c 'falling back' "$SPLIT_LOG" || true; }
bmi_hash() { shasum -a 256 "$1/CMakeFiles/mathlib.dir/mathlib.pcm" | cut -c1-12; }

configure() {  # configure <dir> [launcher...]
    local dir="$1"; shift
    rm -rf "$dir"
    cmake -GNinja -S "$PROJECT" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_CXX_FLAGS=-fmodules-reduced-bmi \
        "$@" >/dev/null 2>&1
}

echo "==> splitter: $SPLITTER"
echo "==> compiler: $($CXX --version | head -1), -fmodules-reduced-bmi, -j$JOBS"
[ -x "$SPLITTER" ] || { echo "no splitter at $SPLITTER"; exit 1; }

printf '\n%-14s %9s %9s %7s   %-14s %-14s %s\n' scenario plain split ratio 'plain objs/cc' 'split objs/cc' 'BMI'
printf '%-14s %9s %9s %7s   %-14s %-14s %s\n' -------------- --------- --------- ------- -------------- -------------- ---
report() {  # report <label> <plain-ms> <split-ms> <bmi-note>
    printf '%-14s %9s %9s %6sx   %-14s %-14s %s\n' "$1" "$(human "$2")" "$(human "$3")" "$(ratio_of "$2" "$3")" \
        "$(objects "$PLAIN_LOG")/$(compiles "$PLAIN_LOG")" "$(objects "$SPLIT_LOG")/$(compiles "$SPLIT_LOG")" "$4"
}
bmi_note() {  # bmi_note <plain-before> <plain-after> <split-before> <split-after>
    local p s
    p=$([ "$1" = "$2" ] && echo same || echo changed)
    s=$([ "$3" = "$4" ] && echo same || echo changed)
    echo "plain $p, split $s"
}

for run in $(seq 1 "$RUNS"); do
    [ "$RUNS" -gt 1 ] && echo "-- run $run --"
    cp "$MODULE_BACKUP" "$MODULE"; cp "$IMPORTER_BACKUP" "$IMPORTER"

    configure "$PLAIN"
    configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$SPLITTER"
    report "full" "$(build "$PLAIN")" "$(build "$SPLIT")" ""
    if [ "$(fallbacks)" -ne 0 ]; then
        echo; echo "    ABORT: $(fallbacks) translation unit(s) fell back to plain compilation."
        grep -m3 -B6 'falling back' "$SPLIT_LOG" | grep -E 'error' | head -3; exit 1
    fi
    build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
    report "no-op" "$(build "$PLAIN")" "$(build "$SPLIT")" ""

    touch "$IMPORTER"; p=$(build "$PLAIN")
    touch "$IMPORTER"; s=$(build "$SPLIT")
    report "one source" "$p" "$s" ""

    pb=$(bmi_hash "$PLAIN"); sb=$(bmi_hash "$SPLIT")
    touch "$MODULE"; p=$(build "$PLAIN")
    touch "$MODULE"; s=$(build "$SPLIT")
    report "one module" "$p" "$s" "$(bmi_note "$pb" "$(bmi_hash "$PLAIN")" "$sb" "$(bmi_hash "$SPLIT")")"

    pb=$(bmi_hash "$PLAIN"); sb=$(bmi_hash "$SPLIT")
    cp "$MODULE_BACKUP" "$MODULE"; patch_body "$MODULE" "export int compute_17(int n) {" "$((run * 4))"
    p=$(build "$PLAIN")
    cp "$MODULE_BACKUP" "$MODULE"; patch_body "$MODULE" "export int compute_17(int n) {" "$((run * 4 + 1))"
    s=$(build "$SPLIT")
    report "one body" "$p" "$s" "$(bmi_note "$pb" "$(bmi_hash "$PLAIN")" "$sb" "$(bmi_hash "$SPLIT")")"
    # Settled again before the next edit, or the restore of this one is part of it.
    cp "$MODULE_BACKUP" "$MODULE"; build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null

    pb=$(bmi_hash "$PLAIN"); sb=$(bmi_hash "$SPLIT")
    cp "$MODULE_BACKUP" "$MODULE"; patch_body_sameline "$((run * 4))"
    p=$(build "$PLAIN")
    cp "$MODULE_BACKUP" "$MODULE"; patch_body_sameline "$((run * 4 + 1))"
    s=$(build "$SPLIT")
    report "one body =" "$p" "$s" "$(bmi_note "$pb" "$(bmi_hash "$PLAIN")" "$sb" "$(bmi_hash "$SPLIT")")"
    cp "$MODULE_BACKUP" "$MODULE"; build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null

    pb=$(bmi_hash "$PLAIN"); sb=$(bmi_hash "$SPLIT")
    cp "$MODULE_BACKUP" "$MODULE"; patch_inline "$((run * 4 + 2))"
    p=$(build "$PLAIN")
    cp "$MODULE_BACKUP" "$MODULE"; patch_inline "$((run * 4 + 3))"
    s=$(build "$SPLIT")
    report "inline body" "$p" "$s" "$(bmi_note "$pb" "$(bmi_hash "$PLAIN")" "$sb" "$(bmi_hash "$SPLIT")")"
    cp "$MODULE_BACKUP" "$MODULE"; build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null

    cp "$IMPORTER_BACKUP" "$IMPORTER"; patch_body "$IMPORTER" "int use_1(int n) {" "$((run * 4))"
    p=$(build "$PLAIN")
    cp "$IMPORTER_BACKUP" "$IMPORTER"; patch_body "$IMPORTER" "int use_1(int n) {" "$((run * 4 + 1))"
    s=$(build "$SPLIT")
    report "importer body" "$p" "$s" ""
    cp "$IMPORTER_BACKUP" "$IMPORTER"
done

echo
echo "==> fallbacks during the last split build: $(fallbacks)"
echo "==> output"
build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
if diff <("$PLAIN/bench") <("$SPLIT/bench") >/dev/null 2>&1; then
    echo "    split and plain print the same thing: $("$SPLIT/bench")"
else
    echo "    MISMATCH: the split program does not agree with the plain one"
fi
echo "==> artefacts"
for name in plain split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || echo "$SPLIT")
    printf '    %-6s binary %-8s build tree %s\n' "$name" "$(du -h "$dir/bench" | cut -f1)" "$(du -sh "$dir" | cut -f1)"
done
echo "    split pieces generated: $(find "$SPLIT" -name '*.cpp' -path '*.split*' | wc -l | tr -d ' ')"
echo "    BMI: plain $(du -h "$PLAIN/CMakeFiles/mathlib.dir/mathlib.pcm" | cut -f1), split $(du -h "$SPLIT/CMakeFiles/mathlib.dir/mathlib.pcm" | cut -f1)"
