export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
tipi run clang++ -std=c++17 src/main.cpp -I/Users/daminetreg/workspace/tipi/hermetic-fetchcontent.release-archive/build/_deps/Boost-install/include -I/usr/local/share/.tipi/clang/a7e6968/include -L/usr/local/share/.tipi/clang/a7e6968/lib/ -Wl,-rpath,/usr/local/share/.tipi/clang/a7e6968/lib/ -lclang  -o cpp-splitter


# On linux

```sh
mkdir -p ../`whoami`-tipi-workdir-vT.w
mkdir -p ../generalized-toolchains
docker run --init --detach --name `whoami`-tipi  -u`id -u`:`id -g` --group-add tipi -e TIPI_CACHE_CONSUME_ONLY=ON -e TIPI_CACHE_FORCE_ENABLE=OFF -e HOME -v $HOME:$HOME:rw \
  --mount type=bind,source=$PWD/../`whoami`-tipi-workdir-vT.w,target=/usr/local/share/.tipi/vT.w/ \
  --mount type=bind,source=$PWD/../generalized-toolchains,target=/usr/local/share/.tipi/environments/generalized/v1/ \
  -v $PWD:$PWD:rw -w $PWD \
  tipibuild/tipi-ubuntu-2404:v0.0.82 \
  sleep infinity

docker exec -u 0 `whoami`-tipi useradd -d $HOME -u `id -u` `whoami`

# This launches a container interactive shell
docker exec -it `whoami`-tipi tipi run /bin/bash
```

## Compile splitter
```sh
g++ -std=c++17 -Wall -Wextra -O2 src/main.cpp -o cpp-splitter -I /usr/local/share/.tipi/clang/4f846ee/include/ -lclang -L /usr/local/share/.tipi/clang/4f846ee/lib -Wl,-rpath,/usr/local/share/.tipi/clang/4f846ee/lib
```

## RBE
```sh
export RBE_service=opal.cluster.engflow.com:443
export RBE_tls_client_auth_key=$HOME/engflow-mTLS/engflow.key
export RBE_tls_client_auth_cert=$HOME/engflow-mTLS/engflow.crt

# Remote caching only not remote execution
export RBE_exec_strategy=local

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

# Generate a new UUID for the session
export RBE_invocation_id=$(uuidgen)

export RBE_proxy_log_dir=$PWD

# Configure + Build remote ccache'd
export CMAKE_C_COMPILER_LAUNCHER="/home/daminetreg/workspace/cpp-splitter/cpp-splitter;tipi-compiler-driver"
export CMAKE_CXX_COMPILER_LAUNCHER="/home/daminetreg/workspace/cpp-splitter/cpp-splitter;tipi-compiler-driver"

bootstrap -server_address $RBE_server_address -shutdown
bootstrap -server_address $RBE_server_address -logtostderr -v 43
cmake -S . -B ./build -G Ninja

# Enable caching post configure (configure checks are randomly stored, do generally not benefit from caching)
export TIPI_INTERCALATED_COMPILER_LAUNCHER=rewrapper

cmake --build ./build-cached -j12 --target clean
cmake --build ./build-cached -j12
bootstrap -server_address $RBE_server_address -shutdown

echo "EngFlow Build Profile: https://${RBE_service}/api/profiling/v1/instances/${RBE_instance:-default}/invocations/${RBE_invocation_id}"


```