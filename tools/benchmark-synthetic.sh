#!/usr/bin/env bash
#
# A generated corpus of perfectly splittable code -- UNITS translation units of FUNCS free
# functions each, example/synthetic/generate.py -- built three ways on this machine: plain,
# as a unity build (-DCMAKE_UNITY_BUILD=ON, batches of 8) and through cpp-splitter, in three
# scenarios:
#
#   full      cold: configure, then build. Only the build is timed.
#   no-op     build again with nothing changed.
#   one body  a line added to the body of one function of one unit. Plain recompiles that
#             unit, unity the batch holding it, the splitter re-slices one piece.
#
# UNITS=200 FUNCS=40 by default; MODES chooses the configurations. TODO/52.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNITS="${UNITS:-200}"
FUNCS="${FUNCS:-40}"
JOBS="${JOBS:-16}"
MODES="${MODES:-plain unity split}"
SRC="${SYNTHETIC_DIR:-/tmp/synthetic-src}"
PLAIN=/tmp/synthetic-plain
UNITY=/tmp/synthetic-unity
SPLIT=/tmp/synthetic-split
SPLIT_LOG=/tmp/synthetic-split.log
# The body edited: a function in the middle of a unit in the middle of the corpus.
BODY_UNIT="$SRC/unit_$((UNITS / 2)).cpp"
BODY_FN="unit_$((UNITS / 2))_fn_$((FUNCS / 2))"

rm -rf "$SRC"
python3 "$REPO/example/synthetic/generate.py" "$UNITS" "$FUNCS" "$SRC"
BODY_BACKUP="$(mktemp)"
cp "$BODY_UNIT" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_UNIT"; rm -f "$BODY_BACKUP"' EXIT

patch_body() {  # patch_body <marker>: one line more in the body of BODY_FN
    python3 - "$BODY_UNIT" "$BODY_FN" "$1" <<'PROBE'
import sys
path, fn, marker = sys.argv[1], sys.argv[2], sys.argv[3]
signature = "int %s(int x)\n{\n" % fn
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "    (void)%s;  // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PROBE
}

ms() { date +%s%3N; }
human() { if [ "$1" = "-" ]; then printf "%9s" "-"; else awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; fi; }
has_mode() { case " $MODES " in *" $1 "*) return 0;; *) return 1;; esac; }
ratio_of() { if [ "$1" = "-" ] || [ "$2" = "-" ]; then printf "-"; else awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; fi; }
fallbacks() { [ -r "$SPLIT_LOG" ] || { echo 0; return; }; grep -c 'falling back' "$SPLIT_LOG" || true; }
declined() { [ -r "$SPLIT_LOG" ] || { echo 0; return; }; grep -c '\[cpp-splitter\] not splitting ' "$SPLIT_LOG" || true; }

build() {  # build <dir> -> elapsed ms of the build alone
    local dir="$1" start end log
    log="$dir.log"
    [ "$dir" = "$SPLIT" ] && log="$SPLIT_LOG"
    start=$(ms)
    CPP_SPLITTER_VERBOSE=1 tipi run ninja -C "$dir" -j"$JOBS" > "$log" 2>&1 \
        || { echo "BUILD FAILED in $dir, see $log" >&2; exit 1; }
    end=$(ms)
    echo $((end - start))
}

configure() {  # configure <dir> [cmake args...]
    local dir="$1"; shift
    rm -rf "$dir"
    tipi run cmake -GNinja -S "$SRC" -B "$dir" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        "$@" > "$dir.cfg.log" 2>&1 || { echo "CONFIGURE FAILED in $dir, see $dir.cfg.log" >&2; exit 1; }
}

echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j32 >/dev/null
echo "==> $UNITS units of $FUNCS functions, Release, -j$JOBS, build phase only"

printf '\n%-12s %9s %9s %9s %11s %11s %11s %9s %8s\n' scenario plain unity split plain/unity plain/split unity/split fallbacks declined
printf '%-12s %9s %9s %9s %11s %11s %11s %9s %8s\n' ------------ --------- --------- --------- ----------- ----------- ----------- --------- --------
report() {
    printf '%-12s %9s %9s %9s %10sx %10sx %10sx %9s %8s\n' "$1" "$(human "$2")" "$(human "$3")" "$(human "$4")" \
        "$(ratio_of "$3" "$2")" "$(ratio_of "$4" "$2")" "$(ratio_of "$4" "$3")" "$(fallbacks)" "$(declined)"
}
build_mode() {
    case "$1" in
        plain) has_mode plain && build "$PLAIN" || echo "-" ;;
        unity) has_mode unity && build "$UNITY" || echo "-" ;;
        split) has_mode split && build "$SPLIT" || echo "-" ;;
    esac
}
has_mode plain && configure "$PLAIN"
has_mode unity && configure "$UNITY" -DCMAKE_UNITY_BUILD=ON
has_mode split && configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"
p=$(build_mode plain); u=$(build_mode unity); s=$(build_mode split)
report "full" "$p" "$u" "$s"
for l in "$PLAIN.log" "$UNITY.log" "$SPLIT_LOG"; do [ -r "$l" ] && cp "$l" "$l.full"; done
for d in "$PLAIN" "$UNITY" "$SPLIT"; do
    [ -x "$d/synthetic" ] && echo "==> $(basename "$d") prints $("$d/synthetic")"
done

build_mode plain >/dev/null; build_mode unity >/dev/null; build_mode split >/dev/null
report "no-op" "$(build_mode plain)" "$(build_mode unity)" "$(build_mode split)"

cp "$BODY_BACKUP" "$BODY_UNIT"; patch_body 2; p=$(build_mode plain)
cp "$BODY_BACKUP" "$BODY_UNIT"; patch_body 3; u=$(build_mode unity)
cp "$BODY_BACKUP" "$BODY_UNIT"; patch_body 4; s=$(build_mode split)
report "one body" "$p" "$u" "$s"
cp "$BODY_BACKUP" "$BODY_UNIT"

echo
echo "==> full build: $(grep -c 'Building CXX' "$PLAIN.log.full" 2>/dev/null || true) compiles plain; $(grep -c 'Building CXX' "$UNITY.log.full" 2>/dev/null || true) unity, of which $(grep 'Building CXX' "$UNITY.log.full" 2>/dev/null | grep -c 'unity_' || true) batches; split: $(grep -c 'Building CXX' "$SPLIT_LOG.full" 2>/dev/null || true) units, $(grep 'compiling [0-9]* split file' "$SPLIT_LOG.full" 2>/dev/null | awk '{s+=$3} END{print s+0}') + $(grep -c 'compile (seq)' "$SPLIT_LOG.full" 2>/dev/null || true) pieces, $(grep 'compiling [0-9]* shared' "$SPLIT_LOG.full" 2>/dev/null | awk '{s+=$3} END{print s+0}') shared, $(grep -c 'PCH built' "$SPLIT_LOG.full" 2>/dev/null || true) PCH"
if has_mode split; then
    echo "==> one body: plain $(grep -c 'Building CXX' "$PLAIN.log" || true) compile(s); unity $(grep -c 'Building CXX' "$UNITY.log" || true); split $(grep -c 're-sliced its piece' "$SPLIT_LOG" || true) re-sliced, $(grep 'compiling [0-9]* split file' "$SPLIT_LOG" | awk '{s+=$3} END{print s+0}') + $(grep -c 'compile (seq)' "$SPLIT_LOG" || true) piece compile(s), $(grep -c 'PCH built' "$SPLIT_LOG" || true) PCH"
fi
