# C++20 named modules with CMake

The example from Kitware's *import CMake; the Experiment is Over!*
(https://www.kitware.com/import-cmake-the-experiment-is-over/), reproduced as published:
a module interface unit `foo.cxx` in a library, imported by `main.cxx`. TODO/43 is the
design for splitting module units; this is the baseline it measures against.

## Requirements (from the post)

- CMake 3.28 or newer
- One of: Visual Studio 2022 17.4+ (MSVC toolset 14.34), LLVM/Clang 16.0+,
  GCC 14 (a 2023-09-20 daily bump or newer)
- The Ninja generator or a Visual Studio generator

CMake asks the compiler for the module dependency graph before compiling
(`clang-scan-deps` / `gcc -fdeps-format=p1689r5`), so the tipi clang 13 in
`environments/monolithic.cmake` cannot build this; it has no `-fmodule-output`.

## Build (as in the post)

```sh
mkdir build && cd build
CXX=clang++ CC=clang cmake -GNinja ..
ninja -v
./hello          # hello world
```

## Build on this repository's machines

Apple's command-line tools ship no `clang-scan-deps`, so CMake refuses modules with the
`AppleClang` of `environments/macos-apple-clang.cmake` ("the compiler does not provide a way
to discover the import graph dependencies"). Homebrew's llvm has both the compiler and the
scanner; it needs the SDK named explicitly:

```sh
CXX=$(brew --prefix llvm)/bin/clang++ CC=$(brew --prefix llvm)/bin/clang \
  cmake -GNinja -S example/cpp-20-modules -B example/cpp-20-modules/tmp/plain \
        -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)
ninja -C example/cpp-20-modules/tmp/plain -v && example/cpp-20-modules/tmp/plain/hello
```

`ninja -v` shows what CMake adds for modules: `clang-scan-deps -format=p1689` per unit
writing a `.ddi`, a `CXXModules.json` collation, then each compile given a response file
`@CMakeFiles/foo.dir/foo.cxx.o.modmap` -- `-x c++-module -fmodule-output=…/foo.pcm` for
the interface unit, `-fmodule-file=foo=…/foo.pcm` for the importer.

## In the test suite

`example.cpp_20_modules` configures, builds and runs this with the suite's compiler, plain
(the launcher does not take module units yet, TODO/43), and reports a CTest *skip* where
CMake cannot drive modules -- clang < 16, Apple's clang, CMake < 3.28. To run it on macOS,
name the compiler at configure time:

```sh
cmake-re --host -S . -B build/cmake-re-macos-apple-clang \
         -DCMAKE_TOOLCHAIN_FILE=environments/macos-apple-clang.cmake \
         -Dcpp_splitter_modules_cxx=$(brew --prefix llvm)/bin/clang++
ctest-re --test-dir build/cmake-re-macos-apple-clang -R example.cpp_20_modules --output-on-failure
```

## What a BMI holds, and when it changes

`bmi-probe/probe.sh` measures, on a small module, what `-fmodules-reduced-bmi` drops
(non-inline bodies, in-class member bodies included) and shows that the reduced BMI still
changes on every body edit through the definition's ODR hash in the `DECL_FUNCTION` record,
while an interface that only declares the function stays byte-identical. The numbers are in
TODO/43.

```sh
CXX=$(brew --prefix llvm)/bin/clang++ SDKROOT=$(xcrun --show-sdk-path) example/cpp-20-modules/bmi-probe/probe.sh
```
