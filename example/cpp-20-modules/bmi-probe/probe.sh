#!/usr/bin/env bash
# What a BMI holds and when its bytes change. Backs TODO/43. Needs clang >= 20
# (-fmodules-reduced-bmi); on macOS Homebrew's llvm and the SDK:
#   CXX=$(brew --prefix llvm)/bin/clang++ SDKROOT=$(xcrun --show-sdk-path) example/cpp-20-modules/bmi-probe/probe.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
CXX=${CXX:-clang++}
LLVM=$(dirname "$(command -v "$CXX")")
flags=(-std=c++20 ${SDKROOT:+-isysroot "$SDKROOT"})
work=${1:-$here/tmp}; rm -rf "$work"; mkdir -p "$work"; cd "$work"

# $1 dir, $2 interface source (copied to one fixed path, so the BMI's file records do not
# differ by name), $3.. extra flags for the interface compile. Prints the BMI's hash.
bmi() { local d=$1 src=$2; shift 2; mkdir -p "$d"; cp "$src" "$work/math.cppm"
  "$CXX" "${flags[@]}" -x c++-module -c "$work/math.cppm" -fmodule-output="$d/math.pcm" -o "$d/math.o" -Xclang -fno-pch-timestamp "$@"
  shasum -a 256 "$d/math.pcm" | cut -c1-12; }
bodies() { "$LLVM/llvm-bcanalyzer" -dump "$1" | grep -c STMT_COMPOUND || true; }
same() { [ "$1" = "$2" ] && echo IDENTICAL || echo DIFFERS; }

echo "== 1. what the BMI holds (STMT_COMPOUND records = function bodies serialised)"
bmi full "$here/math.cppm" >/dev/null;  echo "full:    $(stat -f %z full/math.pcm 2>/dev/null || stat -c %s full/math.pcm) bytes, bodies=$(bodies full/math.pcm)"
bmi reduced "$here/math.cppm" -fmodules-reduced-bmi >/dev/null; echo "reduced: $(stat -f %z reduced/math.pcm 2>/dev/null || stat -c %s reduced/math.pcm) bytes, bodies=$(bodies reduced/math.pcm)"
"$CXX" "${flags[@]}" -fmodule-file=math=reduced/math.pcm -c "$here/math_impl.cpp" -o reduced/math_impl.o
"$CXX" "${flags[@]}" -fmodule-file=math=reduced/math.pcm -c "$here/use.cpp" -o reduced/use.o
"$CXX" "${flags[@]}" reduced/math.o reduced/math_impl.o reduced/use.o -o reduced/prog; echo "program against the reduced BMI: $(reduced/prog)   (expected 42 7 7 8)"

echo "== 2. does the reduced BMI change on an edit? (reference = unedited interface)"
ref=$(bmi r0 "$here/math.cppm" -fmodules-reduced-bmi)
sleep 1;                                                   echo "rebuilt later, no edit:            $(same "$ref" "$(bmi r0 "$here/math.cppm" -fmodules-reduced-bmi)")"
sed 's/return 7; }/return 8; }/' "$here/math.cppm" > e1.cppm; echo "non-inline body, same length:      $(same "$ref" "$(bmi r1 e1.cppm -fmodules-reduced-bmi)")"
sed 's/helper_kept(); }/helper_kept() * 10; }/' "$here/math.cppm" > e2.cppm; echo "exported non-inline body:          $(same "$ref" "$(bmi r2 e2.cppm -fmodules-reduced-bmi)")"
sed 's/return 5; }/return 6; }/' "$here/math.cppm" > e3.cppm; echo "out-of-line member body:           $(same "$ref" "$(bmi r3 e3.cppm -fmodules-reduced-bmi)")"
sed 's/v \* 2/v + v/' "$here/math.cppm" > e4.cppm;         echo "inline body:                       $(same "$ref" "$(bmi r4 e4.cppm -fmodules-reduced-bmi)")"
"$LLVM/llvm-bcanalyzer" -dump r0/math.pcm > r0.txt; "$LLVM/llvm-bcanalyzer" -dump r1/math.pcm > r1.txt
echo "records differing after the same-length edit: $(diff r0.txt r1.txt | grep -c '^<') -> $(diff r0.txt r1.txt | grep '^<' | grep -o '<[A-Z_]*' | sort -u | tr '\n' ' ')"

echo "== 3. the split shape: the body out of the interface, in an implementation unit"
sed 's/export int seven() { return helper_kept(); }/export int seven();/' "$here/math.cppm" > s0.cppm
sref=$(bmi s0 s0.cppm -fmodules-reduced-bmi)
{ echo 'module math;'; echo 'int seven() { return helper_kept() * 10; }'; } > seven_piece.cpp   # the edit lives here now
echo "interface rebuilt with the body edited elsewhere: $(same "$sref" "$(bmi s1 s0.cppm -fmodules-reduced-bmi)")"
