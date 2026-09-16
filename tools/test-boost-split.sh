cd /home/daminetreg/workspace/cpp-splitter

#tipi run cmake -GNinja -S . -B build \
#  -DCMAKE_BUILD_TYPE=Debug \
#  -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake
tipi run cmake --build build -j32

# 2. Build Boost.Filesystem through the splitter

REPO=/home/daminetreg/workspace/cpp-splitter
OUT=/tmp/boost-split

#rm -rf "$OUT"
#tipi run cmake -GNinja -S "$REPO/example/boost-to-split" -B "$OUT" \
#  -DCMAKE_BUILD_TYPE=Debug \
#  -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" \
#  -DBOOST_INCLUDE_LIBRARIES=filesystem \
#  -DBUILD_SHARED_LIBS=OFF \
#  -DCMAKE_CXX_COMPILER_LAUNCHER="$REPO/build/cpp-splitter"

cd "$OUT"
CPP_SPLITTER_VERBOSE=1 tipi run ninja -d explain -j8 2>&1 | tee split-build.log