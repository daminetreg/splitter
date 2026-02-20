#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SPLITTER="$REPO_ROOT/cpp-splitter"

pushd $REPO_ROOT
  g++ -std=c++17 -Wall -Wextra -O2 src/main.cpp -o $SPLITTER  -I/usr/lib/llvm-18/include/ -lclang -L/usr/lib/llvm-18/lib/ -Wl,-rpath,/usr/lib/llvm-18/lib/
popd

BUILD_DIR="$SCRIPT_DIR/build"

if [ ! -x "$SPLITTER" ]; then
    echo "Error: cpp-splitter not found at $SPLITTER"
    echo "Run 'make' in the repo root first."
    exit 1
fi



#rm -rf "$BUILD_DIR"
#mkdir -p "$BUILD_DIR"

# RBE
export RBE_service=opal.cluster.engflow.com:443
export RBE_tls_client_auth_key=$HOME/engflow-mTLS/engflow.key
export RBE_tls_client_auth_cert=$HOME/engflow-mTLS/engflow.crt

# Configure scandeps cache
mkdir -p $PWD/.scandeps_cache
export RBE_cache_dir=$PWD/.scandeps_cache
export RBE_deps_cache_max_mb=512
export RBE_enable_deps_cache="true"

export RBE_exec_root=$PWD
export RBE_platform="InputRootAbsolutePath=$PWD,container-image=docker://tipibuild/tipi-ubuntu-2404@sha256:5441e0ae56f6bdbd915c42ce7d84ad3f84787a62b8c487e77aa935ee7acb47e5"
export RBE_server_address=unix://$PWD/unix-sock-reproxy
export RBE_canonicalize_working_dir="False"
export RBE_reproxy_wait_seconds=5
export RBE_service_no_auth="true"
export RBE_use_application_default_credentials="true"

export RBE_compression_threshold=0
export RBE_rpc_timeouts="BatchUpdateBlobs=0,BatchReadBlobs=0,Read=0,Write=0,GetTree=0,default=0"
export RBE_grpc_keepalive_timeout=5s
export RBE_grpc_keepalive_time=15s
export RBE_compression_threshold=0
export RBE_use_batches=false
export RBE_use_unified_downloads=true
export RBE_use_unified_uploads=true

# Generate a new UUID for the session
export RBE_invocation_id=$(uuidgen)
export RBE_exec_strategy="remote"

mkdir -p $SCRIPT_DIR/logs
export RBE_proxy_log_dir=$SCRIPT_DIR/logs

export CPP_SPLITTER_VERBOSE=on
#echo "=== Starting cpp-splitter server ===src/main.cpp"
#"$SPLITTER" --server &
#SERVER_PID=$!
#sleep 0.5
#trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT
export CPP_SPLITTER_NO_SERVER=1

# Configure + Build remote ccache'd
export CMAKE_C_COMPILER_LAUNCHER="/home/daminetreg/workspace/cpp-splitter/cpp-splitter;tipi-compiler-driver"
export CMAKE_CXX_COMPILER_LAUNCHER="/home/daminetreg/workspace/cpp-splitter/cpp-splitter;tipi-compiler-driver"

echo "=== Starting bootstrap ==="
bootstrap -server_address $RBE_server_address -shutdown
bootstrap -server_address $RBE_server_address -logtostderr -v 43
trap "bootstrap -server_address $RBE_server_address -shutdown" EXIT


echo ""
echo "=== Configuring with CMake (cpp-splitter as launcher) ==="
cmake \
    -DCMAKE_C_COMPILER=/usr/local/share/.tipi/clang/4f846ee/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/local/share/.tipi/clang/4f846ee/bin/clang++ \
    --debug-trycompile \
    -G Ninja \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR"

export TIPI_INTERCALATED_COMPILER_LAUNCHER=rewrapper

echo ""
echo "=== Building ==="
export CMAKE_BUILD_PARALLEL_LEVEL=300
VERBOSE=1 cmake --build "$BUILD_DIR" -j300


echo ""
echo "=== Running the built binary ==="
#"$BUILD_DIR/spirit_example"

