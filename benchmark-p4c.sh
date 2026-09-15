#!/usr/bin/env bash
#
# p4c (github.com/p4lang/p4c) built three ways on this machine -- plain, as a unity build
# (-DCMAKE_UNITY_BUILD=ON), and through cpp-splitter -- in the five scenarios the other
# benchmarks use:
#
#   full         cold: configure, then build. Only the build is timed.
#   no-op        build again with nothing changed.
#   one source   touch frontends/p4/callGraph.cpp. Timestamp only.
#   one header   touch lib/cstring.h, which every unit includes. Timestamp only.
#   one body     change the body of cstring::size() in lib/cstring.h.
#
# The corpus is p4c's own code without the control plane (which would pull Protobuf and
# the backends that need it) and without the GTest suite: lib, ir, frontends, midend, the
# p4fmt and graphs backends and the ir-generator, plus Abseil, which p4c fetches and builds
# itself. Abseil is built plain in every configuration: p4c keeps it out of the unity build
# (cmake/Abseil.cmake), and the launcher is withheld from it the same way, through
# CMAKE_PROJECT_absl_INCLUDE. TODO/47.
#
# p4c needs C++20; clang 13 cannot compile libstdc++ 13's <chrono> in C++20 mode, so the
# build uses the toolchain's libc++, linked dynamically (lib/backtrace_exception.cpp redefines
# libstdc++'s std::__throw_* under __GLIBC__, which lld rejects against the static libc++.a).
# The dependencies are set up under build/p4c-deps without root: Boost 1.85 built from
# example/boost-to-split with libc++, and bison, flex, m4, libgc and libgmp unpacked from
# their Ubuntu 24.04 packages.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P4C="$REPO/example/p4c"
P4C_REV=8192431
JOBS="${JOBS:-16}"
DEPS="$REPO/build/p4c-deps"
CLANG_ROOT=/usr/local/share/.tipi/clang/4f846ee
PLAIN=/tmp/p4c-plain
UNITY=/tmp/p4c-unity
SPLIT=/tmp/p4c-split
SPLIT_LOG=/tmp/p4c-split.log
SOURCE="$P4C/frontends/p4/callGraph.cpp"
HEADER="$P4C/lib/cstring.h"
BODY_HEADER="$P4C/lib/cstring.h"
BODY_BACKUP="$(mktemp)"

if [ ! -d "$P4C/frontends" ]; then
    echo "==> cloning p4c $P4C_REV into example/p4c"
    git clone -q --depth 1 https://github.com/p4lang/p4c.git "$P4C"
    git -C "$P4C" checkout -q "$P4C_REV" 2>/dev/null || true
fi

# --- dependencies, without root ------------------------------------------------------------
if [ ! -x "$DEPS/root/usr/bin/bison" ]; then
    echo "==> unpacking bison, flex, m4, libgc and libgmp into $DEPS/root"
    mkdir -p "$DEPS/debs"
    (cd "$DEPS/debs" && apt-get download bison m4 flex libfl-dev libfl2 libgc-dev libgc1 \
        libgmp-dev libgmp10 libgmpxx4ldbl >/dev/null)
    for d in "$DEPS"/debs/*.deb; do dpkg-deb -x "$d" "$DEPS/root"; done
fi
if [ ! -f "$DEPS/boost/lib/libboost_iostreams.a" ]; then
    echo "==> building Boost 1.85 (iostreams, graph, format, multiprecision) with libc++"
    rm -rf "$REPO/build/p4c-boost"
    tipi run cmake -GNinja -S "$REPO/example/boost-to-split" -B "$REPO/build/p4c-boost" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
        "-DCMAKE_CXX_FLAGS=-stdlib=libc++" \
        "-DBOOST_INCLUDE_LIBRARIES=iostreams;graph;format;multiprecision;functional" \
        -DBUILD_SHARED_LIBS=OFF -DBOOST_IOSTREAMS_ENABLE_ZLIB=OFF -DBOOST_IOSTREAMS_ENABLE_BZIP2=OFF \
        -DBOOST_IOSTREAMS_ENABLE_LZMA=OFF -DBOOST_IOSTREAMS_ENABLE_ZSTD=OFF \
        -DCMAKE_INSTALL_PREFIX="$DEPS/boost" >/dev/null 2>&1
    tipi run cmake --build "$REPO/build/p4c-boost" -j32 >/dev/null 2>&1
    tipi run cmake --install "$REPO/build/p4c-boost" >/dev/null 2>&1
fi
export PATH="$DEPS/root/usr/bin:$PATH"
export BISON_PKGDATADIR="$DEPS/root/usr/share/bison" M4="$DEPS/root/usr/bin/m4"
export LD_LIBRARY_PATH="$DEPS/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ABSL_PLAIN="$DEPS/absl-plain.cmake"
printf '# Injected after Abseil'"'"'s project(): built without the launcher in every configuration.\nset(CMAKE_CXX_COMPILER_LAUNCHER "")\n' > "$ABSL_PLAIN"

cp "$BODY_HEADER" "$BODY_BACKUP"
trap 'cp "$BODY_BACKUP" "$BODY_HEADER"; rm -f "$BODY_BACKUP"' EXIT

patch_body() {
    python3 - "$BODY_HEADER" "$1" <<'PROBE'
import sys
path, marker = sys.argv[1], sys.argv[2]
signature = "    size_t size() const {\n"
src = open(path).read()
assert src.count(signature) == 1, "benchmark probe target moved"
src = src.replace(signature, signature + "        (void)%s;  // benchmark probe\n" % marker, 1)
open(path, "w").write(src)
PROBE
}

ms() { date +%s%3N; }
human() { awk -v v="$1" 'BEGIN { printf "%6.1fs", v/1000 }'; }
ratio_of() { awk -v a="$1" -v b="$2" 'BEGIN { if (a > 0) printf "%.2f", b/a; else printf "-" }'; }
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

# The values with spaces and semicolons go through an initial cache file: `tipi run` re-splits
# its arguments and breaks them on the command line.
CACHE_INIT="$DEPS/p4c-cache.cmake"
cat > "$CACHE_INIT" <<EOF
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_TOOLCHAIN_FILE "$REPO/environments/monolithic.cmake" CACHE FILEPATH "")
set(CMAKE_CXX_FLAGS "-stdlib=libc++" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++ -L$CLANG_ROOT/lib -Wl,-rpath,$CLANG_ROOT/lib" CACHE STRING "")
set(CMAKE_PREFIX_PATH "$DEPS/boost;$DEPS/root/usr" CACHE STRING "")
set(CMAKE_INCLUDE_PATH "$DEPS/root/usr/include" CACHE STRING "")
set(CMAKE_LIBRARY_PATH "$DEPS/root/usr/lib/x86_64-linux-gnu" CACHE STRING "")
set(ENABLE_CONTROL_PLANE OFF CACHE BOOL "")
set(ENABLE_GTESTS OFF CACHE BOOL "")
set(BUILD_LINK_WITH_LLD ON CACHE BOOL "")
EOF

configure() {  # configure <dir> [cmake args...]
    local dir="$1"; shift
    rm -rf "$dir"
    tipi run cmake -GNinja -S "$P4C" -B "$dir" -C "$CACHE_INIT" \
        "$@" > "$dir.cfg.log" 2>&1 || { echo "CONFIGURE FAILED in $dir, see $dir.cfg.log" >&2; exit 1; }
}

echo "==> building cpp-splitter"
tipi run cmake --build "$REPO/build" -j32 >/dev/null
echo "==> p4c $(git -C "$P4C" log -1 --format=%h), no control plane, no tests, Release, -j$JOBS, build phase only"

printf '\n%-12s %9s %9s %9s %11s %11s %9s %8s\n' scenario plain unity split unity/plain split/plain fallbacks declined
printf '%-12s %9s %9s %9s %11s %11s %9s %8s\n' ------------ --------- --------- --------- ----------- ----------- --------- --------
report() {
    printf '%-12s %9s %9s %9s %10sx %10sx %9s %8s\n' "$1" "$(human "$2")" "$(human "$3")" "$(human "$4")" \
        "$(ratio_of "$2" "$3")" "$(ratio_of "$2" "$4")" "$(fallbacks)" "$(declined)"
}

configure "$PLAIN"
configure "$UNITY" -DCMAKE_UNITY_BUILD=ON
configure "$SPLIT" -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter" \
                   -DCMAKE_PROJECT_absl_INCLUDE="$ABSL_PLAIN"
p=$(build "$PLAIN"); u=$(build "$UNITY"); s=$(build "$SPLIT")
report "full" "$p" "$u" "$s"
cp "$SPLIT_LOG" "$SPLIT_LOG.full"
full_units=$(grep -c 'Building CXX' "$SPLIT_LOG.full" || true)
plain_units=$(grep -c 'Building CXX' "$PLAIN.log" || true)
unity_compiles=$(grep -c 'Building CXX' "$UNITY.log" || true)
unity_batches=$(grep 'Building CXX' "$UNITY.log" | grep -c 'unity_' || true)

build "$PLAIN" >/dev/null; build "$UNITY" >/dev/null; build "$SPLIT" >/dev/null
report "no-op" "$(build "$PLAIN")" "$(build "$UNITY")" "$(build "$SPLIT")"

touch "$SOURCE"; p=$(build "$PLAIN")
touch "$SOURCE"; u=$(build "$UNITY")
touch "$SOURCE"; s=$(build "$SPLIT")
report "one source" "$p" "$u" "$s"

touch "$HEADER"; p=$(build "$PLAIN")
touch "$HEADER"; u=$(build "$UNITY")
touch "$HEADER"; s=$(build "$SPLIT")
report "one header" "$p" "$u" "$s"

cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body 2; p=$(build "$PLAIN")
cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body 3; u=$(build "$UNITY")
cp "$BODY_BACKUP" "$BODY_HEADER"; patch_body 4; s=$(build "$SPLIT")
report "one body" "$p" "$u" "$s"
body_resliced=$(grep -c 're-sliced its piece' "$SPLIT_LOG" || true)
body_parsed=$(grep -c '^\[cpp-splitter\] libclang args:' "$SPLIT_LOG" || true)
body_refusals=$(grep -oE '\[cpp-splitter\] full split: .*' "$SPLIT_LOG" | sort | uniq -c | sort -rn | head -5 || true)
cp "$BODY_BACKUP" "$BODY_HEADER"

echo
echo "==> full build: $plain_units C++ compiles plain (Abseil: $(grep 'Building CXX' "$PLAIN.log" | grep -c abseil || true)); unity $unity_compiles, of which $unity_batches unity batches of p4c sources"
echo "==> one body: $body_resliced unit(s) re-sliced, $body_parsed re-parsed"
[ -n "$body_refusals" ] && echo "$body_refusals" | sed 's/^/    /'

echo
echo "==> fallbacks and declines on the full split build, of $full_units compiles: $(grep -c 'falling back' "$SPLIT_LOG.full" || true) fell back, $(grep -c '\[cpp-splitter\] not splitting ' "$SPLIT_LOG.full" || true) declined"
python3 - "$SPLIT_LOG.full" <<'PY'
import sys, re, collections
lines = open(sys.argv[1], errors="replace").read().split("\n")
cats = collections.Counter()
for l in lines:
    if "[cpp-splitter] not splitting " in l:
        cats["declined: " + l.split(": ", 2)[-1][:100]] += 1
remaining = sum(1 for l in lines if "falling back" in l)
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
echo "==> programs"
sample="$P4C/testdata/p4_16_samples/arith-bmv2.p4"
for name in plain unity split; do
    dir=$([ "$name" = plain ] && echo "$PLAIN" || { [ "$name" = unity ] && echo "$UNITY" || echo "$SPLIT"; })
    printf '    %-6s p4fmt %s  p4c-graphs %s\n' "$name" \
        "$("$dir/p4fmt" "$sample" | md5sum | cut -c1-12)" \
        "$("$dir/p4c-graphs" --version 2>&1 | head -1 | cut -c1-60)"
done
echo "    build tree: plain $(du -sh "$PLAIN" | cut -f1), unity $(du -sh "$UNITY" | cut -f1), split $(du -sh "$SPLIT" | cut -f1)"
echo "    split pieces generated: $(find "$SPLIT" -name '*.cpp' -path '*.split*' | wc -l)"
