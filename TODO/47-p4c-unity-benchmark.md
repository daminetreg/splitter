# 47 — p4c: plain, unity build, split

## Motivation

Every corpus so far was compared plain against split. A unity build (`CMAKE_UNITY_BUILD=ON`,
several sources concatenated into one translation unit) is the usual answer to a slow C++
build, and its trade is the opposite of the splitter's: fewer, larger units make the cold
build faster and every incremental build slower, since an edit recompiles the whole batch.
p4c (github.com/p4lang/p4c) documents `-DCMAKE_UNITY_BUILD=ON` as a supported way to build
it, so the three can be measured on one corpus that was written with unity builds in mind.

## Implementation Proposal

`benchmark-p4c.sh`, after `benchmark-opencv.sh`: three configurations of the same source
tree -- plain, `-DCMAKE_UNITY_BUILD=ON`, and plain with `cpp-splitter` as the compiler
launcher -- built with clang 13 in Release, `-j16`, the build phase timed, over the five
scenarios (full, no-op, one source touched, one header touched, one function body edited in
a header). The corpus is p4c's own code without the control plane (`ENABLE_CONTROL_PLANE=OFF`,
which also removes the bmv2/ebpf/dpdk/tc/p4test backends that depend on it, and Protobuf)
and without the GTest suite: `lib`, `ir`, `frontends`, `midend`, the `p4fmt` and `graphs`
backends and the ir-generator -- 218 C++ units -- plus the 102 units of Abseil, which p4c
fetches and builds itself and excludes from the unity build (`cmake/Abseil.cmake`). Abseil
is in all three builds and is never unity-batched, so it adds the same cost to each.

p4c requires C++20. clang 13 cannot compile libstdc++ 13's `<chrono>` in C++20 mode
(`consteval` in `hh_mm_ss`), so the build uses the toolchain's libc++, linked dynamically:
`lib/backtrace_exception.cpp` redefines libstdc++'s `std::__throw_*` under `__GLIBC__`, which
lld rejects as duplicates against the static `libc++.a`. Boost 1.85 is built from
`example/boost-to-split` with libc++ into `build/p4c-deps/boost`; bison, flex, m4, libgc and
libgmp are unpacked from their Ubuntu 24.04 packages into `build/p4c-deps/root`, no root
needed (`bison` is told its data directory and its `m4` through `BISON_PKGDATADIR` and `M4`).
The script sets all of that up when absent.

Probes: `SOURCE` one frontend source; `HEADER` a header every unit includes; `BODY` a line
added inside a function body of such a header, chosen from the split tree so that one unit
emits the function as a piece and every unit includes the header.

## Acceptance Criteria

- One run of `benchmark-p4c.sh` prints the five rows for the three configurations with
  the fallback and decline counts of the split build, the unit and unity-batch counts, and
  the body row's re-slice count.
- The split build has 0 fallbacks and 0 declined, or every one is listed with its reason.
- `p4fmt` and `p4c-graphs` from each build format and version-print the same.
- Results in `benchmarks/p4c-unity.md`, with the unity batch count and what an edit costs
  in each configuration.
