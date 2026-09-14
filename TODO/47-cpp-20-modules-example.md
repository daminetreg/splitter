# 47 — `example/cpp-20-modules`: Kitware's C++20 modules example, as written

## Motivation

TODO/43 designs how the splitter should handle named modules, and needs a reference build
to measure against. The smallest one that the CMake maintainers stand behind is the example
in Kitware's post *import CMake; the Experiment is Over!*
(https://www.kitware.com/import-cmake-the-experiment-is-over/): one interface unit, one
importer, `FILE_SET CXX_MODULES`, CMake ≥ 3.28 with Ninja. Having it in the tree, byte for
byte as published, gives a known-good baseline to point the launcher at before anything from
TODO/43 lands, and a place to see which of the repository's toolchains can drive modules at
all (the tipi clang 13 cannot: no `-fmodule-output`, see TODO/43).

## Implementation Proposal

- `example/cpp-20-modules/`: `foo.cxx`, `main.cxx`, `CMakeLists.txt` copied from the post
  unchanged, and a `README.md` with the post's version requirements and build commands, plus
  the same commands against this repository's toolchain files.
- `example.cpp_20_modules` in CTest, `test/cmake/RunCpp20ModulesExample.cmake`: configures
  the example with the suite's compiler and Ninja, builds it, runs `hello` and expects
  `hello world`. Plain build only — the launcher does not take module units yet (TODO/43
  Phase 0). Skips with the `cpp-splitter-test-skip:` marker when CMake is older than 3.28,
  the generator is not Ninja, or the compiler cannot write a BMI (`-fmodule-output`), so the
  clang 13 toolchain reports a skip rather than a failure.

## Acceptance Criteria

- `cmake -GNinja -S example/cpp-20-modules -B <dir> && ninja -C <dir> && <dir>/hello` prints
  `hello world` with Apple clang and with the ubuntu-clang toolchain's compiler when it is
  clang ≥ 16.
- `ctest -R example.cpp_20_modules` passes on those toolchains and reports *skipped* on the
  tipi clang 13 toolchain.
