# cpp-splitter

`cpp-splitter` reads a C++ translation unit through libclang and rewrites it as one piece per
function definition behind a shared preamble, so the pieces compile in parallel -- locally or
on a remote build execution cluster -- and are linked back (`ld -r`) into the single object the
build system asked for. It runs standalone on one file, or transparently as a
`CMAKE_CXX_COMPILER_LAUNCHER` in front of the real compiler; see [DOCS.md](DOCS.md) and
[blog.md](blog.md) for how and why.

## Building

Both hosts build through [CMake RE](https://tipi.build/documentation/0000-getting-started-cmake)
(`cmake-re`, ships `ctest-re`), which brings its own cmake, ninja and clang 13:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/tipi-build/cli/master/install/install_for_macos_linux.sh)"
export TIPI_DISABLE_AR_RANLIB_DRIVER=ON TIPI_CACHE_CONSUME_ONLY=ON TIPI_CACHE_FORCE_ENABLE=OFF
```

The first configure fetches and builds Boost 1.85 through HermeticFetchContent and takes a few
minutes; later ones reuse it.

### macOS

Host build with the clang 13 tipi installs at `/usr/local/share/.tipi/clang/a7e6968`
(x86_64; the installer sets up Rosetta on Apple silicon):

```sh
cmake-re --host -S . -B build/cmake-re-macos-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/macos-clang.cmake
cmake-re --build build/cmake-re-macos-clang --host -j8
ctest-re --test-dir build/cmake-re-macos-clang --output-on-failure -j8
```

The same Mac can also run the Linux build below: `cmake-re` starts the image through Docker
Desktop (>= 27.2.0) and mounts the tree into it.

### Linux

Containerized in `tipibuild/tipi-ubuntu-2404:v0.0.87`, the image `environments/ubuntu-clang.*`
pins by digest -- the same one CI, the devcontainer and the benchmarks use. Needs docker
>= 27.2.0:

```sh
cmake-re -S . -B build/cmake-re-ubuntu-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/ubuntu-clang.cmake
cmake-re --build build/cmake-re-ubuntu-clang -j8
cmake-re --build build/cmake-re-ubuntu-clang --run-test all --test-jobs 8
```

The tests run inside the container through `--run-test`: `ctest-re` executes on the host,
where a Linux `cpp-splitter` cannot.

Already inside that image (the devcontainer, or `tipi run /bin/bash` in a container started by
hand -- see [CLAUDE.md](CLAUDE.md)), plain cmake works too:

```sh
cmake -GNinja -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake
cmake --build build -j8
ctest --test-dir build --output-on-failure -j8
```

`--build` must be the first argument to `cmake-re`. `build/<name>` is a symlink into
cmake-re's mirror of the tree. `launcher.remote_split_on_opal` skips unless it finds EngFlow
mTLS credentials in `~/engflow-mTLS` (or `ENGFLOW_MTLS_DIR`) on a Linux host.

## Using it

Standalone, split one file, compile the pieces and link them:

```sh
./build/cmake-re-ubuntu-clang/cpp-splitter path/to/unit.cpp out/ --compile -o out/program --cxx clang++
```

As a compiler launcher in any CMake project:

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=/abs/path/to/cpp-splitter
```

Behind `cmake-re --distributed`, the split itself runs on the RBE cluster; TODO/35 and
`build-spirit-cmake-re.sh` show the setup. Results of the Boost measurements are in
`benchmarks/`.
