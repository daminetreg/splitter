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
#   ./build-spirit-cmake-re.sh --distributed --split --clean   # force a cold build
#   CMAKE_RE_JOBS=200 ./build-spirit-cmake-re.sh --distributed
#
# RBE credentials are mTLS: the cluster authenticates the client by its certificate, so there
# are no per-RPC credentials and nothing here reads a token. Point ENGFLOW_MTLS_DIR at a
# directory holding the certificate and key if they are not in ~/engflow-mTLS.
#
# `--distributed` implies `--host` here, and the pair needs no docker. The two flags answer
# different questions -- where the build is orchestrated, and where the compiles run -- so
# asking for both means orchestrate locally and execute on the cluster.
#
# What would need docker is resolving the environment image: cmake-re has to tell the workers
# which image to run in, and turning a tag into a manifest digest is what it shells out for.
# environments/ubuntu-clang.pkr.js pins the digest instead, so that step disappears. Re-pin it
# when the image moves:
#
#   curl -fsSI -H "Authorization: Bearer $TOKEN" \
#        -H "Accept: application/vnd.docker.distribution.manifest.v2+json" \
#        https://registry-1.docker.io/v2/tipibuild/tipi-ubuntu-2404/manifests/<tag>
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIGIN="$REPO/example/spirit-tests"

# Which cmake-re to drive.
#
# Putting cpp-splitter in front of the compiler needs a cmake-re that understands
# CMAKE_<LANG>_COMPILER_LAUNCHER and CMAKE_<LANG>_LINKER_LAUNCHER: it sets a launcher of its
# own (tipi-compiler-driver, which is how the action reaches the cluster), so a build that also
# wants the splitter needs the two composed rather than one overwriting the other. v0.0.87 on
# PATH does not do that; the binary in cmake-re-dev-latest/ does. Prefer it when it is there.
# Debug unless told otherwise. The build type is part of every action key, so changing it
# invalidates the whole RBE cache -- which is a nuisance when comparing runs and a gift when a
# cold measurement is what is wanted.
BUILD_TYPE="${BUILD_TYPE:-Debug}"

CMAKE_RE="${CMAKE_RE:-}"
if [ -z "$CMAKE_RE" ]; then
    if [ -x "$REPO/cmake-re-dev-latest/cmake-re" ]; then
        CMAKE_RE="$REPO/cmake-re-dev-latest/cmake-re"
    else
        CMAKE_RE="$(command -v cmake-re || true)"
    fi
fi
SOURCE="$REPO/example/boost-to-split/cmake-re"
USE_SPLITTER=0
REMOTE_SPLIT=0
DISTRIBUTED=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --split) USE_SPLITTER=1 ;;
        # TODO/35: produce the split on the cluster instead of on this machine. Only means
        # anything together with --split --distributed, since it needs cpp-splitter in the
        # build and reproxy's environment to borrow.
        --remote-split) REMOTE_SPLIT=1 ;;
        --distributed) DISTRIBUTED=1 ;;
        --clean) CLEAN=1 ;;
        --host) ;;   # the default, and accepted so the two can be written together
        --help|-h) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# `--host` and `--distributed` are not alternatives: the first says where the build is
# orchestrated from and the second says where the compiles run. Together they mean orchestrate
# here, execute on the cluster, which is the combination that needs no docker locally.
MODE_FLAGS=(--host)
MODE=host
if [ "$DISTRIBUTED" = 1 ]; then
    MODE_FLAGS+=(--distributed)
    MODE=distributed
fi

BUILD="$REPO/build/cmake-re-spirit-$MODE"
[ "$USE_SPLITTER" = 1 ] && BUILD="$BUILD-split"

# EngFlow RBE, over mTLS.
#
# cmake-re drives the distribution with reclient, which takes its configuration from
# environment variables named RBE_<flag> after reproxy's own flags -- so these names are
# reproxy's, not this script's invention.
#
# service_no_auth is not a weakening: the cluster authenticates the client by its certificate
# during the TLS handshake, so there are no per-RPC credentials to send, and this is what turns
# those off. TLS itself stays on; disabling it would be RBE_service_no_security, which is not
# set here and should not be.
#
# Nothing sets RBE_use_application_default_credentials. Setting it to false looked tidier and
# was inert -- reproxy logged `--use_application_default_credentials=true` regardless, because
# service_no_auth had already disabled per-RPC credentials. An export with no effect is worse
# than no export, so it is gone.
ENGFLOW_MTLS_DIR="${ENGFLOW_MTLS_DIR:-$HOME/engflow-mTLS}"
if [ "$DISTRIBUTED" = 1 ]; then
    export RBE_service="${RBE_service:-opal.cluster.engflow.com:443}"
    export RBE_tls_client_auth_cert="${RBE_tls_client_auth_cert:-$ENGFLOW_MTLS_DIR/engflow.crt}"
    export RBE_tls_client_auth_key="${RBE_tls_client_auth_key:-$ENGFLOW_MTLS_DIR/engflow.key}"
    export RBE_service_no_auth="${RBE_service_no_auth:-true}"
    # Per-action records, so a run can be audited afterwards rather than taken on trust:
    #   reclient/<rev>/dumpstats --proxy_log_dir=<dir> --output_dir=<dir>
    # writes rbe_metrics.txt with the remote, cache-hit and local-fallback counts. Without it
    # reproxy keeps no record of where each action ran.
    export RBE_proxy_log_dir="${RBE_proxy_log_dir:-$REPO/build/rbe-logs}"
    mkdir -p "$RBE_proxy_log_dir"

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
if [ "$DISTRIBUTED" = 1 ]; then
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

if [ -z "$CMAKE_RE" ] || [ ! -x "$CMAKE_RE" ]; then
    echo "cmake-re not found. Install it with:" >&2
    echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/tipi-build/cli/master/install/install_for_macos_linux.sh)"' >&2
    echo "or set CMAKE_RE to a binary." >&2
    exit 1
fi
echo "==> cmake-re:     $CMAKE_RE ($("$CMAKE_RE" --info 2>/dev/null | awk '/^version:/ {print $2}'))"

# Only reported, never required: --host does not use docker. It is what --remote and
# --distributed would need, and knowing it is absent explains why this script uses --host.
if command -v docker >/dev/null 2>&1 && docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    echo "==> docker server: $(docker version --format '{{.Server.Version}}' 2>/dev/null)"
    docker_ok=1
else
    echo "==> docker server: unavailable"
    docker_ok=0
fi

# No docker check, and no docker needed. cmake-re shells out to docker to resolve a tagged
# image reference into the manifest digest it must give the RBE workers;
# environments/ubuntu-clang.pkr.js names the digest outright, so there is nothing to resolve.
# Building the environment locally -- which is what --remote would do -- still needs docker.

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

# cmake-re runs compiles, links and archiving through helpers found on PATH by bare name, and
# the image ships them non-executable for anyone but the `tipi` user. tools/stage-tipi-drivers.sh
# takes copies we own; it explains the mode in full and is shared with the ctest suite. The
# copies are ordinary files in the build tree and cost about 190 MB.
SHIM_DIR="$REPO/build/.tipi-drivers"
prepare_drivers() {
    local home shim
    home="$("$CMAKE_RE" --info 2>/dev/null | awk '/^tipi_home_dir:/ {print $2}')"
    shim="$("$REPO/tools/stage-tipi-drivers.sh" "$SHIM_DIR" ${home:+"$home"})"
    if [ -n "$shim" ]; then
        echo "==> shimmed tipi driver(s) into $shim (not group-executable in the image)"
        PATH="$shim:$PATH"
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
    # C++ only: cpp-splitter parses C++ with libclang and has nothing to do with C sources.
    # No LINKER_LAUNCHER either -- the splitter does its own `ld -r` inside the compile step,
    # and the final link is an ordinary one.
    launcher_args+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$REPO/build/cpp-splitter")
fi

if [ "$REMOTE_SPLIT" = 1 ]; then
    if [ "$USE_SPLITTER" != 1 ] || [ "$DISTRIBUTED" != 1 ]; then
        echo "--remote-split needs --split and --distributed" >&2
        exit 2
    fi
    export CPP_SPLITTER_REMOTE_SPLIT=1
fi

# Say which of the four paths each unit took. Without this the splitter is silent, and a row
# cannot be attributed at all: a measurement labelled "split on the cluster" that quietly
# declined and split here instead looks exactly like one that worked, only slower. It costs a
# few lines per unit in the build log and nothing in wall time.
if [ "$USE_SPLITTER" = 1 ]; then
    export CPP_SPLITTER_VERBOSE=1
fi

echo "==> source:    $SOURCE"
echo "==> build dir: $BUILD"
echo "==> build type: $BUILD_TYPE"
echo "==> jobs:      $JOBS"
echo "==> mode:      ${MODE_FLAGS[*]}"
echo "==> splitter:  $([ "$USE_SPLITTER" = 1 ] && echo yes || echo no)"
echo "==> split on:  $([ "$REMOTE_SPLIT" = 1 ] && echo cluster || echo "this machine")"

# Removing -B is not enough to force a cold build. cmake-re keys its real build directory on
# the configuration, so an identical configure lands back on the same one and ninja reports
# "no work to do" over outputs an earlier run produced -- which, after a run that fell back,
# means a green build measuring nothing. --clean removes the directory -B resolves to.
if [ "$CLEAN" = 1 ]; then
    resolved="$(readlink -f "$BUILD" 2>/dev/null || true)"
    if [ -n "$resolved" ] && [ -d "$resolved" ]; then
        echo "==> clean: removing $resolved"
        rm -rf "$resolved"
    fi
    rm -rf "$BUILD"
fi

echo "==> configure"
"$CMAKE_RE" "${MODE_FLAGS[@]}" \
    -S "$SOURCE" \
    -B "$BUILD" \
    -j "$JOBS" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/environments/ubuntu-clang.cmake" \
    "${launcher_args[@]}"

# `--build` has to be the first argument: cmake-re dispatches on it to pick the subcommand, and
# putting --host ahead of it is a usage error rather than a flag ordering nicety.
#
# Timed here rather than around this whole script, and reported on a line the benchmark parses.
# Everything above -- copying the project in, staging the drivers, mirroring the sources,
# configuring -- is real cost but it is not the build, and on this corpus it is about 20s that
# was landing in every measured row.
echo "==> build"
build_start_ms="$(date +%s%3N)"
build_status=0
# `|| build_status=$?` rather than reading $? on the next line: this script runs under `set -e`,
# which would exit before the assignment and take the timing line with it.
"$CMAKE_RE" --build "$BUILD" "${MODE_FLAGS[@]}" -j "$JOBS" || build_status=$?
echo "==> build wall ms: $(( $(date +%s%3N) - build_start_ms ))"
[ "$build_status" -eq 0 ] || exit "$build_status"

# -B is a symlink into the mirror rather than a directory, so anything reporting on it has to
# resolve it first. cmake-re builds out of its own copy of the source tree, which is why the
# compile lines name paths under $TIPI_HOME/vT.w and not the working tree.
OUT="$(readlink -f "$BUILD")"
echo
echo "==> build output:        $OUT"
echo "==> test programs built: $(find "$OUT" -maxdepth 1 -type f -executable -name 'spirit_test_*' 2>/dev/null | wc -l)"
echo "==> build tree:          $(du -sh "$OUT" 2>/dev/null | cut -f1)"
