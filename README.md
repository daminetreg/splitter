# cpp-splitter

[![cmake-re](https://github.com/daminetreg/splitter/actions/workflows/cmake-re.yml/badge.svg?branch=main)](https://github.com/daminetreg/splitter/actions/workflows/cmake-re.yml)

`cpp-splitter` reads a C++ translation unit through libclang and rewrites it as one piece per
function definition behind a shared preamble, so the pieces compile in parallel (locally or
on a remote build execution cluster) and are linked back (`{mold,ld,lld} -r`) into the single object the
build system asked for. It runs transparently as a `CMAKE_CXX_COMPILER_LAUNCHER` in front of the real compiler; see [DOCS.md](DOCS.md) and
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

<details>
<summary><b>macOS</b> — host build with tipi's clang, Apple's or Homebrew's, and the Linux image through Docker</summary>


Host build with the clang 13 tipi installs at `/usr/local/share/.tipi/clang/a7e6968`
(x86_64; the installer sets up Rosetta on Apple silicon):

```sh
cmake-re --host -S . -B build/cmake-re-macos-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/macos-clang.cmake
cmake-re --build build/cmake-re-macos-clang --host -j8
ctest-re --test-dir build/cmake-re-macos-clang --output-on-failure -j8
```

Or with the clang macOS ships, which is what CI does since the installer does not bring
tipi's clang to a GitHub runner. Apple's toolchain lacks the `clang-c/` headers, so libclang
comes from Homebrew's llvm:

```sh
brew install llvm
cmake-re --host -S . -B build/cmake-re-macos-apple-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/macos-apple-clang.cmake
cmake-re --build build/cmake-re-macos-apple-clang --host -j8
ctest-re --test-dir build/cmake-re-macos-apple-clang --output-on-failure -j8
```

Or entirely with Homebrew's LLVM — its clang driving the compiles and its libclang parsing.
This is the build for C++20 named modules (TODO/43): Apple's clang has no `clang-scan-deps`
for CMake's module scan, and `-fmodules-reduced-bmi` needs clang >= 20. The toolchain file
points the compiler at the macOS SDK itself, since a non-Apple clang does not find it:

```sh
brew install llvm
cmake-re --host -S . -B build/cmake-re-macos-brew-llvm -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/macos-brew-llvm.cmake
cmake-re --build build/cmake-re-macos-brew-llvm --host -j8
ctest-re --test-dir build/cmake-re-macos-brew-llvm --output-on-failure -j8
```

`launcher.module_interface_split` and `example.cpp_20_modules` run here and skip on the two
builds above. `./benchmark-cpp20-modules.sh` uses this splitter by default.

The same Mac can also run the Linux build below: `cmake-re` starts the image through Docker
Desktop (>= 27.2.0) and mounts the tree into it.

</details>

<details>
<summary><b>Linux</b> — in the tipi image, through cmake-re or plain cmake</summary>


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

Already inside that image (the devcontainer, a GitHub `container:` job, or `tipi run /bin/bash`
in a container started by hand -- see [CLAUDE.md](CLAUDE.md)), build on the host instead, which
is what CI does:

```sh
cmake-re --host -S . -B build/cmake-re-ubuntu-clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/ubuntu-clang.cmake
cmake-re --build build/cmake-re-ubuntu-clang --host -j8
ctest-re --test-dir build/cmake-re-ubuntu-clang --output-on-failure -j8
```

Plain cmake works there too:

```sh
cmake -GNinja -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake
cmake --build build -j8
ctest --test-dir build --output-on-failure -j8
```

`--build` must be the first argument to `cmake-re`. `build/<name>` is a symlink into
cmake-re's mirror of the tree. `launcher.remote_split_on_rbe` runs only on a Linux host with
cmake-re >= v0.0.88 and `RBE_service`, `RBE_tls_client_auth_cert` and `RBE_tls_client_auth_key`
set -- the variables reclient reads, the last two naming the EngFlow mTLS pair -- and skips
otherwise. CI sets them from the `RBE_SERVICE`, `RBE_TLS_CLIENT_AUTH_CERT` and
`RBE_TLS_CLIENT_AUTH_KEY` secrets.

</details>

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
