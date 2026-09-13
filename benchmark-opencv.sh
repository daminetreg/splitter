#!/usr/bin/env bash
#
# A minimal OpenCV -- the core and imgproc modules, static, nothing optional -- built plain and
# through cpp-splitter, on this machine, in the five scenarios the Boost benchmarks use:
#
#   full         cold: configure, then build. Only the build is timed.
#   no-op        build again with nothing changed.
#   one source   touch modules/core/src/arithm.cpp. Timestamp only.
#   one header   touch opencv2/core/cvdef.h, which every unit includes. Timestamp only.
#   one body     change the body of SparseMat::nzcount() in opencv2/core/mat.inl.hpp. Every
#                unit includes that header; one emits the function. A plain build recompiles
#                every includer; a split build recompiles the one piece. (Chosen from the split
#                tree: the least-emitted function in a header all 158 units include.)
#
# OpenCV is a corpus of a different shape from Boost's test suites: .cpp files of hundreds to
# thousands of lines sharing a set of headers every unit includes. It is also the first corpus
# here with C-API definitions (CV_IMPL) and a CPU-dispatch macro layer, and both make the
# splitter fall back on some units. The count is reported per row and the causes summarised
# at the end; a fallback compiles the unit whole, so the build stays correct and the row stays
# a measurement of the tool as it is.
#
# Usage:
#   ./benchmark-opencv.sh              # clones opencv 4.11.0 into example/opencv if absent
#   JOBS=8 ./benchmark-opencv.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENCV="$REPO/example/opencv"
OPENCV_TAG=4.11.0
JOBS="${JOBS:-16}"
PLAIN=/tmp/opencv-plain
SPLIT=/tmp/opencv-split
SPLIT_LOG=/tmp/opencv-split.log

SOURCE="$OPENCV/modules/core/src/arithm.cpp"
HEADER="$OPENCV/modules/core/include/opencv2/core/cvdef.h"
BODY_HEADER="$OPENCV/modules/core/include/opencv2/core/mat.inl.hpp"
BODY_BACKUP="$(mktemp)"

if [ ! -d "$OPENCV/modules/core" ]; then
    echo "==> cloning opencv $OPENCV_TAG into example/opencv"
    git clone -q --depth 1 --branch "$OPENCV_TAG" https://github.com/opencv/opencv.git "$OPENCV"
fi
cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP"' EXIT

patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "size_t SparseMat::nzcount() const\n{\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "    (void)%s;  // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PROBE
}

ms() { date +%s%3N; }
human() { awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; }
fallbacks() {
    # grep -c exits 1 on zero matches, which under `||` printed the count and then 0 again.
    [ -r "$SPLIT_LOG" ] || { echo 0; return; }
    grep -c 'falling back' "$SPLIT_LOG" || true
}

build() {  # build <dir> -> elapsed ms of the build alone
    local dir="$1" start end log=/dev/null
    [ "$dir" = "$SPLIT" ] && log="$SPLIT_LOG"
    start=$(ms)
    CPP_SPLITTER_VERBOSE=1 tipi run ninja -C "$dir" -j"$JOBS" > "$log" 2>&1 \
        || { echo "BUILD FAILED in $dir, see $log" >&2; exit 1; }
    end=$(ms)
    if [ "$dir" = "$SPLIT" ] && grep -q '^Killed' "$SPLIT_LOG"; then
        echo "OOM kills in $dir -- this is not a measurement" >&2
        exit 1
    fi
    echo $((end - start))
}

# core and imgproc, static, and nothing optional: no tests, apps, bindings, IPP, OpenCL,
# threading libraries, image codecs or CPU-dispatch variants. The unit set is the two
# modules' own sources.
configure() {  # configure <dir> [launcher...]
    local dir="$1"; shift
    rm -rf "$dir"
    tipi run cmake -GNinja -S "$OPENCV" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        -DBUILD_LIST=core,imgproc -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF \
        -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF \
        -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF -DWITH_EIGEN=OFF \
        -DWITH_ITT=OFF -DWITH_PROTOBUF=OFF -DWITH_ADE=OFF -DWITH_CAROTENE=OFF -DWITH_LAPACK=OFF \
        -DWITH_OPENEXR=OFF -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF \
        -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF \
        -DWITH_V4L=OFF -DWITH_GTK=OFF -DWITH_1394=OFF \
        -DCPU_DISPATCH= -DOPENCV_GENERATE_PKGCONFIG=OFF -DBUILD_ZLIB=ON \
        "$@" >/dev/null 2>&1
}

echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j32 >/dev/null
echo "==> opencv $(git -C "$OPENCV" log -1 --format=%h), core + imgproc, Release, -j$JOBS, build phase only"

printf '\n%-14s %10s %10s %10s %10s\n' scenario plain split ratio fallbacks
printf '%-14s %10s %10s %10s %10s\n' -------------- ---------- ---------- ---------- ----------
report() {
    printf '%-14s %10s %10s %9sx %10s\n' "$1" "$(human "$2")" "$(human "$3")" \
        "$(ratio_of "$2" "$3")" "$(fallbacks)"
}

configure "$PLAIN"
configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"
p=$(build "$PLAIN"); s=$(build "$SPLIT")
report "full" "$p" "$s"
full_units=$(grep -c 'Building CXX' "$SPLIT_LOG" || true)
cp "$SPLIT_LOG" "$SPLIT_LOG.full"

# Settle both trees before the incremental rows.
build "$PLAIN" >/dev/null; build "$SPLIT" >/dev/null
report "no-op" "$(build "$PLAIN")" "$(build "$SPLIT")"

touch "$SOURCE"; p=$(build "$PLAIN")
touch "$SOURCE"; s=$(build "$SPLIT")
report "one source" "$p" "$s"

touch "$HEADER"; p=$(build "$PLAIN")
touch "$HEADER"; s=$(build "$SPLIT")
report "one header" "$p" "$s"

cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body 2; p=$(build "$PLAIN")
cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body 3; s=$(build "$SPLIT")
report "one body" "$p" "$s"
body_resliced=$(grep -c 're-sliced its piece' "$SPLIT_LOG" || true)
body_parsed=$(grep -c '^\[cpp-splitter\] libclang args:' "$SPLIT_LOG" || true)
body_refusals=$(grep -oE '\[cpp-splitter\] full split: .*' "$SPLIT_LOG" | sort | uniq -c | sort -rn | head -5 || true)
cp "$BODY_BACKUP" "$BODY_HEADER"

echo
echo "==> one body: $body_resliced unit(s) re-sliced, $body_parsed re-parsed"
[ -n "$body_refusals" ] && echo "$body_refusals" | sed 's/^/    /'

echo
echo "==> fallbacks on the full build: $(grep -c 'falling back' "$SPLIT_LOG.full" || true) of $full_units units, by cause"
python3 - "$SPLIT_LOG.full" <<'PY'
import sys, re, collections
lines = open(sys.argv[1], errors="replace").read().split("\n")
cats = collections.Counter()
# Declines say so on a line of their own, one per unit; with -j16 the lines of different
# units interleave, so they are counted from those lines rather than from what precedes a
# fallback. The remaining fallbacks are attributed to the nearest diagnostic above them.
declined = [l for l in lines if "[cpp-splitter] not splitting " in l]
for l in declined:
    cats["declined: " + l.split(": ", 2)[-1][:100]] += 1
remaining = sum(1 for l in lines if "falling back" in l) - len(declined)
for i, l in enumerate(lines):
    if remaining <= 0: break
    if "falling back" not in l or "splitting failed" in l: continue
    block = lines[max(0, i - 400):i]
    if any("relocatable link failed" in x for x in block[-4:]):
        sym = next((re.search(r"multiple definition of `([^']*)'", x) for x in reversed(block) if "multiple definition" in x), None)
        cats["ld -r: multiple definition (e.g. %s)" % (sym.group(1) if sym else "?")] += 1
    else:
        errs = [x for x in block if " error: " in x and "Parse error" not in x]
        msg = re.sub(r"'[^']*'", "'…'", errs[0].split(" error: ", 1)[1])[:80] if errs else "(no diagnostic captured)"
        cats[msg] += 1
    remaining -= 1
for k, v in cats.most_common(): print(f"    {v:3}  {k}")
PY

echo
echo "==> archives"
for name in plain split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || echo "$SPLIT")
    for lib in core imgproc; do
        printf '    %-6s libopencv_%-8s %s bytes\n' "$name" "$lib" "$(stat -c %s "$dir/lib/libopencv_$lib.a")"
    done
done
# The split archive must define the same external symbols as the plain one.
for lib in core imgproc; do
    a=$(nm --defined-only -g "$PLAIN/lib/libopencv_$lib.a" 2>/dev/null | awk '{print $3}' | sort -u)
    b=$(nm --defined-only -g "$SPLIT/lib/libopencv_$lib.a" 2>/dev/null | awk '{print $3}' | sort -u)
    missing=$(comm -23 <(echo "$a") <(echo "$b") | wc -l)
    extra=$(comm -13 <(echo "$a") <(echo "$b") | wc -l)
    printf '    libopencv_%-8s external symbols: plain %s, split %s; missing from split %s, only in split %s\n' \
        "$lib" "$(echo "$a" | wc -l)" "$(echo "$b" | wc -l)" "$missing" "$extra"
done
echo "    build tree: plain $(du -sh "$PLAIN" | cut -f1), split $(du -sh "$SPLIT" | cut -f1)"
echo "    split pieces generated: $(find "$SPLIT" -name '*.cpp' -path '*.split*' | wc -l)"
