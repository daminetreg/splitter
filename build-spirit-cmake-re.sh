#!/usr/bin/env bash
#
# Build Boost.Spirit and its own test suite through CMake RE, on this host.
#
# `--host` means no isolation and no hermeticity: cmake-re drives the build with the toolchain
# and the compiler that are already here. That is the right mode inside the tipi container,
# where this repository is normally worked on -- there is no docker-in-docker to give
# `--remote` an environment to build in, and the container *is* the environment
# environments/ubuntu-clang.pkr.js describes, so isolating from it would be isolating from the
# thing being reproduced.
#
# What gets built is example/boost-to-split/cmake-re: the Boost superproject configured for
# Spirit, plus the 277 test programs example/spirit-tests reads out of Spirit's Jamfiles.
#
# Usage:
#   ./build-spirit-cmake-re.sh              # plain build
#   ./build-spirit-cmake-re.sh --split      # through cpp-splitter as the compiler launcher
#   CMAKE_RE_JOBS=16 ./build-spirit-cmake-re.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIGIN="$REPO/example/spirit-tests"
SOURCE="$REPO/example/boost-to-split/cmake-re"
USE_SPLITTER=0
BUILD="$REPO/build/cmake-re-spirit"

for arg in "$@"; do
    case "$arg" in
        --split) USE_SPLITTER=1; BUILD="$REPO/build/cmake-re-spirit-split" ;;
        --help|-h) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# A Spirit test unit is a very large template instantiation, and with the splitter in front of
# it the launcher holds a libclang AST of that unit alongside the compile. Eight is what
# 122 GiB carries; without the splitter there is no such ceiling.
if [ "$USE_SPLITTER" = 1 ]; then
    JOBS="${CMAKE_RE_JOBS:-8}"
else
    JOBS="${CMAKE_RE_JOBS:-$(nproc)}"
fi

# Required for every cmake-re invocation in this repository. The cache is a shared service we
# read from and do not write to, and the ar/ranlib driver is off because the toolchain named by
# environments/ubuntu-clang.cmake brings its own.
export TIPI_DISABLE_AR_RANLIB_DRIVER="ON"
export TIPI_CACHE_CONSUME_ONLY="ON"
export TIPI_CACHE_FORCE_ENABLE="OFF"

if ! command -v cmake-re >/dev/null 2>&1; then
    echo "cmake-re is not installed. Install it with:" >&2
    echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/tipi-build/cli/master/install/install_for_macos_linux.sh)"' >&2
    exit 1
fi

# Only reported, never required: --host does not use docker. It is what --remote and
# --distributed would need, and knowing it is absent explains why this script uses --host.
if command -v docker >/dev/null 2>&1 && docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    echo "==> docker server: $(docker version --format '{{.Server.Version}}' 2>/dev/null) (--remote available)"
else
    echo "==> docker server: unavailable, so --host is the only mode that can run here"
fi

if [ ! -d "$REPO/example/boost-to-split/libs/spirit" ]; then
    echo "example/boost-to-split does not contain a Boost checkout with libs/spirit" >&2
    exit 1
fi

# Install the project into the Boost checkout.
#
# cmake-re mirrors the git repository enclosing the source tree it is given, and
# example/boost-to-split is a Boost checkout with a .git of its own. A project placed there can
# therefore reach nothing in the cpp-splitter repository around it -- and, for the same reason,
# cpp-splitter cannot track files placed there. So the originals live in example/spirit-tests
# and are copied in on every run: the tracked copy is always the one that gets built, and a
# re-vendored Boost checkout needs no repair beyond running this script again.
mkdir -p "$SOURCE"
cp "$ORIGIN/cmake-re/CMakeLists.txt" "$SOURCE/CMakeLists.txt"
cp "$ORIGIN/SpiritTestsFromJamfiles.cmake" "$SOURCE/SpiritTestsFromJamfiles.cmake"
echo "==> installed the cmake-re project into $SOURCE"

# cmake-re drives the compile, link, ar and ranlib steps through helper binaries under
# $TIPI_HOME/<driver>/<rev>/, referenced by bare name and found on PATH. In
# tipibuild/tipi-ubuntu-2404:v0.0.87 every one of them is mode `-rwxrw-r--` and owned by the
# `tipi` user: the owner may execute them, the group may not. The container recipe this
# repository documents runs as your own uid with `--group-add tipi`, so the group bits are the
# ones that apply and every driver fails with
#
#     /bin/sh: 1: tipi-compiler-driver: Permission denied
#
# Nothing here can fix the mode -- the files belong to another user and we are not root. What
# it can do is take a copy we own, make that executable, and put it first on PATH so the bare
# names resolve to it. The copies are ordinary files in the build tree and cost about 190 MB.
#
# This is a workaround for the image, not for cmake-re, and it is skipped entirely when the
# drivers are already runnable -- as they are when the container runs as the `tipi` user, or
# once the image ships them group-executable.
SHIM_DIR="$REPO/build/.tipi-drivers"
prepare_drivers() {
    local home needed=0 src name
    home="$(cmake-re --info 2>/dev/null | awk '/^tipi_home_dir:/ {print $2}')"
    [ -n "$home" ] || home=/usr/local/share/.tipi

    for src in $(find "$home" -maxdepth 3 -type f -name 'tipi-*-driver' 2>/dev/null); do
        name="$(basename "$src")"
        if [ -x "$src" ]; then
            continue                      # already runnable as us; nothing to shim
        fi
        if [ ! -r "$src" ]; then
            echo "cannot read $src, and it is not executable either -- cmake-re cannot run here" >&2
            exit 1
        fi
        mkdir -p "$SHIM_DIR"
        if [ ! -x "$SHIM_DIR/$name" ] || [ "$src" -nt "$SHIM_DIR/$name" ]; then
            cp "$src" "$SHIM_DIR/$name"
            chmod u+x "$SHIM_DIR/$name"
        fi
        needed=$((needed + 1))
    done

    if [ "$needed" -gt 0 ]; then
        echo "==> shimmed $needed tipi driver(s) into $SHIM_DIR (not group-executable in the image)"
        PATH="$SHIM_DIR:$PATH"
        export PATH
    fi
}
prepare_drivers

launcher_args=()
if [ "$USE_SPLITTER" = 1 ]; then
    if [ ! -x "$REPO/build/cpp-splitter" ]; then
        echo "==> building cpp-splitter first"
        cmake -GNinja -S "$REPO" -B "$REPO/build" \
              -DCMAKE_BUILD_TYPE=Debug \
              -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/monolithic.cmake" >/dev/null
        cmake --build "$REPO/build" -j"$(nproc)" >/dev/null
    fi
    launcher_args+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$REPO/build/cpp-splitter")
fi

echo "==> source:    $SOURCE"
echo "==> build dir: $BUILD"
echo "==> jobs:      $JOBS"
echo "==> splitter:  $([ "$USE_SPLITTER" = 1 ] && echo yes || echo no)"

echo "==> configure"
cmake-re --host \
    -S "$SOURCE" \
    -B "$BUILD" \
    -j "$JOBS" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/ubuntu-clang.cmake" \
    "${launcher_args[@]}"

# `--build` has to be the first argument: cmake-re dispatches on it to pick the subcommand, and
# putting --host ahead of it is a usage error rather than a flag ordering nicety.
echo "==> build"
cmake-re --build "$BUILD" --host -j "$JOBS"

# -B is a symlink into the mirror rather than a directory, so anything reporting on it has to
# resolve it first. cmake-re builds out of its own copy of the source tree, which is why the
# compile lines name paths under $TIPI_HOME/vT.w and not the working tree.
OUT="$(readlink -f "$BUILD")"
echo
echo "==> build output:        $OUT"
echo "==> test programs built: $(find "$OUT" -maxdepth 1 -type f -executable -name 'spirit_test_*' 2>/dev/null | wc -l)"
echo "==> build tree:          $(du -sh "$OUT" 2>/dev/null | cut -f1)"
