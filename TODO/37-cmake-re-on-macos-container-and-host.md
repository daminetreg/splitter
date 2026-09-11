# 37 — cmake-re on macOS: the Linux build in a container, and a host build with tipi's clang

## Motivation

Everything this repository builds is pinned to one Linux image, tipibuild/tipi-ubuntu-2404
v0.0.87, and to the clang that lives at a fixed path inside it. That is the right contract
for the benchmarks and for CI, but it means a macOS machine can only build the splitter by
first entering a container by hand (the `docker run` recipe in CLAUDE.md, or the devcontainer).

cmake-re removes the by-hand part twice over:

- `cmake-re -DCMAKE_TOOLCHAIN_FILE=environments/ubuntu-clang.cmake` builds in that same image,
  started and mounted by cmake-re itself, from a plain macOS shell. The environment files
  already exist (TODO/35 added them for the distributed build); what is missing is the proof
  that they work from a macOS host, and a workflow that runs them.
- `cmake-re --host` builds on the Mac itself. tipi ships a clang 13.0.0 for macOS -- the same
  version as the Linux one, so the libclang API the splitter uses is identical -- reachable at
  `/usr/local/share/.tipi/clang/a7e6968` (the sha1 of the distro archive, as `4f846ee` is on
  Linux). Today CMakeLists.txt cannot use it: the link line hardcodes the Linux toolchain's
  `libc++.a` path and `libclang.so`.

## Implementation Proposal

1. `environments/macos-clang.cmake`: a host toolchain that names tipi's macOS clang by
   absolute path, rejects non-Darwin hosts, and pins C++17 as `ubuntu-clang.cmake` does.
2. CMakeLists.txt: keep the Linux link line as it is, but derive it from `clang_toolchain_root`
   rather than repeating the path, and add the Darwin equivalent -- `libclang.dylib`, the
   toolchain's `lib/` as rpath, and the system libc++ that libclang.dylib is itself linked
   against. On Darwin the tests get `SDKROOT` set from `CMAKE_OSX_SYSROOT`: the drivers call
   `${CXX}` directly, and a clang that is not Apple's finds no `wchar.h` without it.
   The macOS 15+ SDK's math.h also declares `_Float16` helpers unguarded, which clang 13 rejects
   on x86_64; `environments/macos-clang.sdk-shim/math.h` spells the type `float` for that one
   header and the toolchain puts it on `-isystem`. The tests reach it through
   `CPLUS_INCLUDE_PATH`, which clang honours and the splitter's include probe reports.
3. `test/cmake/RunRemoteSplitTest.cmake` skips off Linux: its probe uses the ubuntu-clang
   toolchain through `cmake-re --host`, which refuses a Darwin host.
4. `.github/workflows/cmake-re.yml`, additive next to `build-and-test.yml`:
   - `linux`: ubuntu-latest, install cmake-re, check docker >= 27.2.0, configure and build
     containerized with `environments/ubuntu-clang.cmake`, run the tests in the container
     with `cmake-re --build ... --run-test all` (`ctest-re` v0.0.87 runs on the host, where a
     Linux binary cannot execute).
   - `macos`: macos-latest, install cmake-re, `cmake-re --host` with
     `environments/macos-clang.cmake`, run the tests with `ctest-re`.
5. `TIPI_DISABLE_AR_RANLIB_DRIVER=ON`, `TIPI_CACHE_CONSUME_ONLY=ON`,
   `TIPI_CACHE_FORCE_ENABLE=OFF` exported wherever cmake-re runs, as `build-and-test.yml`
   already does for the last two.
6. Document both invocations in CLAUDE.md next to the existing docker recipe.

## Acceptance Criteria

- From this macOS host, `cmake-re -S . -B build/cmake-re-ubuntu-clang
  -DCMAKE_TOOLCHAIN_FILE=environments/ubuntu-clang.cmake` configures and builds cpp-splitter
  in the container, and `--run-test all` passes the suite there (the same 31 fixtures as
  `build-and-test.yml`; `launcher.remote_split_on_opal` skips without credentials).
- From this macOS host, `cmake-re --host -S . -B build/cmake-re-macos-clang
  -DCMAKE_TOOLCHAIN_FILE=environments/macos-clang.cmake` builds cpp-splitter against tipi's
  clang 13 and `ctest-re` passes the same suite.
- A plain `cmake -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake` build in the tipi
  container still configures with an identical link line (the Linux branch is a refactor, not
  a change).
- `.github/workflows/cmake-re.yml` parses, and `build-and-test.yml` is untouched.
- `example/boost-to-split`: not exercised here. The Boost harness needs the vendored checkout
  and hours; the acceptance for this entry is the unit suite on both hosts.
