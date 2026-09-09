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
# `--distributed` sends the same build to EngFlow's Remote Build Execution cluster instead.
# That is where per-function splitting is meant to pay: a conventional build cannot go faster
# than its slowest translation unit however many workers it is given, and splitting turns each
# unit into hundreds of independent compiles that a farm can take at once.
#
# Usage:
#   ./build-spirit-cmake-re.sh                      # locally, on this host
#   ./build-spirit-cmake-re.sh --split              # through cpp-splitter as compiler launcher
#   ./build-spirit-cmake-re.sh --distributed        # on the RBE cluster
#   ./build-spirit-cmake-re.sh --distributed --split
#   CMAKE_RE_JOBS=200 ./build-spirit-cmake-re.sh --distributed
#
# RBE credentials are mTLS: the cluster authenticates the client by its certificate, so there
# are no per-RPC credentials and nothing here reads a token. Point ENGFLOW_MTLS_DIR at a
# directory holding the certificate and key if they are not in ~/engflow-mTLS.
#
# `--distributed` needs docker on the machine that starts it. Before scheduling anything,
# cmake-re packages the toolchain -- clang, ninja, reclient, the tipi drivers -- and ships it
# so the remote workers execute against the same environment this repository builds in, and it
# calls `docker version` to do that. Inside the tipi container there is no docker-in-docker, so
# the mode has to be started from a host that has one. The credentials and the endpoint are
# fine there; only the packaging step is missing.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIGIN="$REPO/example/spirit-tests"
SOURCE="$REPO/example/boost-to-split/cmake-re"
USE_SPLITTER=0
MODE=host

for arg in "$@"; do
    case "$arg" in
        --split) USE_SPLITTER=1 ;;
        --distributed) MODE=distributed ;;
        --host) MODE=host ;;
        --help|-h) sed -n '2,34p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

BUILD="$REPO/build/cmake-re-spirit-$MODE"
[ "$USE_SPLITTER" = 1 ] && BUILD="$BUILD-split"

# EngFlow RBE, over mTLS.
#
# cmake-re drives the distribution with reclient, which takes its configuration from
# environment variables named RBE_<flag> after reproxy's own flags -- so these names are
# reproxy's, not this script's invention.
#
# service_no_auth is not a weakening: the cluster authenticates the client by its certificate
# during the TLS handshake, so there are no per-RPC credentials to send. TLS itself stays on;
# turning it off would be RBE_service_no_security, which is not set here and should not be.
ENGFLOW_MTLS_DIR="${ENGFLOW_MTLS_DIR:-$HOME/engflow-mTLS}"
if [ "$MODE" = distributed ]; then
    export RBE_service="${RBE_service:-opal.cluster.engflow.com:443}"
    export RBE_tls_client_auth_cert="${RBE_tls_client_auth_cert:-$ENGFLOW_MTLS_DIR/engflow.crt}"
    export RBE_tls_client_auth_key="${RBE_tls_client_auth_key:-$ENGFLOW_MTLS_DIR/engflow.key}"
    export RBE_service_no_auth="${RBE_service_no_auth:-true}"
    export RBE_use_application_default_credentials="${RBE_use_application_default_credentials:-false}"

    for f in "$RBE_tls_client_auth_cert" "$RBE_tls_client_auth_key"; do
        if [ ! -r "$f" ]; then
            echo "RBE credential not readable: $f" >&2
            echo "Set ENGFLOW_MTLS_DIR, or RBE_tls_client_auth_cert/RBE_tls_client_auth_key." >&2
            exit 1
        fi
    done
    # Reported, never echoed: the key is a secret and the certificate identifies the client.
    echo "==> RBE service:  $RBE_service"
    echo "==> RBE identity: $(basename "$RBE_tls_client_auth_cert") (expires $(openssl x509 -noout -enddate -in "$RBE_tls_client_auth_cert" 2>/dev/null | cut -d= -f2 || echo unknown))"
fi

# A Spirit test unit is a very large template instantiation, and with the splitter in front of
# it the launcher holds a libclang AST of that unit alongside the compile. Eight is what
# 122 GiB carries; without the splitter there is no such ceiling.
if [ "$MODE" = distributed ]; then
    # The width is the point of distributing, and it is not bounded by this machine's cores:
    # the compiles run on the cluster and only the scheduling happens here.
    JOBS="${CMAKE_RE_JOBS:-64}"
elif [ "$USE_SPLITTER" = 1 ]; then
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
    echo "==> docker server: $(docker version --format '{{.Server.Version}}' 2>/dev/null)"
    docker_ok=1
else
    echo "==> docker server: unavailable"
    docker_ok=0
fi

# Fail here rather than several minutes in. cmake-re ships the toolchain to the workers before
# it schedules anything and calls `docker version` to package it, so without docker the run
# dies with a bare "execve failed: No such file or directory" after uploading everything.
if [ "$MODE" = distributed ] && [ "$docker_ok" = 0 ]; then
    cat >&2 <<'MSG'
--distributed needs docker on this machine, and there is none.

cmake-re packages the toolchain and ships it to the RBE workers before scheduling any
compile, and it drives that with docker. Inside the tipi container there is no
docker-in-docker, so start this mode from a host that has one:

    ./build-spirit-cmake-re.sh --distributed

The RBE endpoint and the mTLS credentials are unaffected -- they were verified from in here:
the cluster accepts the client certificate over TLS with ALPN h2. Only the packaging step is
missing.

Use --host to build on this machine instead.
MSG
    exit 1
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
echo "==> mode:      --$MODE"
echo "==> splitter:  $([ "$USE_SPLITTER" = 1 ] && echo yes || echo no)"

echo "==> configure"
cmake-re "--$MODE" \
    -S "$SOURCE" \
    -B "$BUILD" \
    -j "$JOBS" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/ubuntu-clang.cmake" \
    "${launcher_args[@]}"

# `--build` has to be the first argument: cmake-re dispatches on it to pick the subcommand, and
# putting --host ahead of it is a usage error rather than a flag ordering nicety.
echo "==> build"
cmake-re --build "$BUILD" "--$MODE" -j "$JOBS"

# -B is a symlink into the mirror rather than a directory, so anything reporting on it has to
# resolve it first. cmake-re builds out of its own copy of the source tree, which is why the
# compile lines name paths under $TIPI_HOME/vT.w and not the working tree.
OUT="$(readlink -f "$BUILD")"
echo
echo "==> build output:        $OUT"
echo "==> test programs built: $(find "$OUT" -maxdepth 1 -type f -executable -name 'spirit_test_*' 2>/dev/null | wc -l)"
echo "==> build tree:          $(du -sh "$OUT" 2>/dev/null | cut -f1)"
