# 38 — a Windows build of the splitter, and a Windows job in CI

Specification only. Nothing here is implemented.

## Motivation

The repository builds and tests on Linux (in the tipi image) and on macOS (tipi's clang or
Apple's), and CI covers both. Windows has nothing, and the reason is not the workflow: a
`windows-latest` job would stop at line 20 of `src/main.cpp`. The splitter was written against
POSIX and a gcc-style driver, and every one of those assumptions has to be met or replaced
before a Windows runner can build it, let alone run a split.

The requested HFC revision, tipi-build/hfc@8065c13 ("Run the option-forwarding tests against
MSVC as well", 2026-08-30), is ten days newer than the 9d56120 pinned today and is the first
one whose own suite passes under a stock Windows box with no gcc or clang on PATH. It is what
lets Boost configure there through HermeticFetchContent; it does nothing for the splitter's
own source.

## What stands in the way, precisely

Inventory of `src/main.cpp` (6069 lines), by category. Line numbers are of `0dc65ee`.

**1. Process spawning through a Unix shell.** Every compile, link and probe is a string built
with `shell_quote()` (l.2709, single-quote escaping) and run through one of:
- `std::system(cmd)` in `run_command()`/`run_command_quiet()` (l.2544, 2549), 10 call sites
  (l.2777, 5312, 5402, 5527, 5538, 5615, 5776, 5789, ...);
- `boost::process::child("/bin/sh", {"-c", cmd})` for the parallel piece compiles (l.2606);
- `popen(cmd)` for the two compiler probes (l.29, l.3885).
On Windows `std::system` goes through `cmd.exe`, whose quoting is not sh's, and `/bin/sh` does
not exist outside an MSYS shell.

**2. Compiler probing.** `detect_system_includes()` (l.26) runs `<cxx> -E -x c++ /dev/null -v`
and parses `#include <...> search starts here:`; the macro probe (l.3884) runs
`<cxx> -x c++ -E -dM /dev/null`. Both need a gcc/clang driver and `/dev/null`.

**3. The relocatable link.** The launcher combines the pieces with `<linker> -r -o out.o ...`
(l.5770), `ld` by default, `CPP_SPLITTER_LINKER` to override (l.5105). Nothing in the MSVC
toolchain does a relocatable link on COFF: `link.exe` and `lld-link` have no `/r`. GNU
`ld -r` does handle COFF objects, so a MinGW-w64 binutils satisfies it; MSVC never will.

**4. POSIX headers and calls.** `<unistd.h>` (l.20); `::access(path, X_OK)` to find drivers on
PATH (l.5095, 5180); `::getpid()` for a temp-file suffix (l.5246);
`fs::read_symlink("/proc/self/exe")` for the splitter's own path (l.5222, remote split only).

**5. Flags and artefacts of a gcc-style driver.** `-MD/-MF/-MMD` depfile handling (l.5378,
5603, and the rewrite in TODO/28's incremental path), `-x c++-header` PCH builds into
`<preamble>.gch/` (l.2769, 2754), `-std=gnu++NN` dialect selection (l.3907), `-include-pch`
for libclang (l.4397). All of these exist in clang on Windows; none exist in `cl.exe`.

**6. The build of the splitter itself.** `CMakeLists.txt` hardcodes `-stdlib=libc++`, links
`libclang${CMAKE_SHARED_LIBRARY_SUFFIX}` from the compiler's own root (or
`CPP_SPLITTER_LIBCLANG_ROOT`), and needs `clang-c/Index.h`. Boost.Process 1.85 compiles on
Windows; Boost.Context's assembly selection is already forwarded for macOS and would need the
Windows equivalent only if the arch were ambiguous (it is not on x86_64 MSYS2).

**7. The test drivers.** `test/cmake/Run*.cmake` call `${CXX}` bare, name outputs `program` and
`<stem>.o` without extensions, run `${program}` directly, and assume `ld` on PATH. The fixtures
themselves are plain C++17 with `<iostream>`; nothing platform-specific.

## Decision: MinGW-w64 through MSYS2, not MSVC

Category 3 decides it. Without a relocatable link there is no launcher mode, and launcher mode
is the product. An MSVC port would have to replace `ld -r` with something else -- a static
library per unit, say -- which changes what the build system receives from the launcher and
would be a different splitter, not a port. It also drags categories 2 and 5 into a second
driver dialect.

Under MSYS2's `CLANG64` environment (`mingw-w64-clang-x86_64-*`), clang is the driver, `ld`
(binutils, or `ld.lld` which also does `-r` on COFF) is on PATH, libclang comes as
`libclang.dll` with `clang-c/` headers from `mingw-w64-clang-x86_64-clang-libs` and
`-tools-extra`, and `<unistd.h>`, `popen`, `getpid`, `access` exist as the mingw-w64 CRT
provides them. Categories 1, 2, 4 and 7 shrink to the concrete edits below; 3 and 5 are met
as they are. The resulting `cpp-splitter.exe` is a native Windows binary that depends on the
MSYS2 clang it was built with -- the same shape as the Linux binary depending on the image's
clang and the macOS one on Homebrew's.

Cost of the decision: a Windows user gets a MinGW clang-driven split, not an MSVC one. That is
the honest scope; the launcher is for clang-compiled projects on Windows as well.

## Implementation Proposal

Ordered so each step is testable on its own, on the Linux and macOS jobs, before the Windows
one exists.

1. **HFC.** Bump `hfc_REVISION` to `8065c13e09624c04b5336ed03fb32d64913276fb` for every
   platform; one revision, not a Windows-only override. Confirm the three existing jobs stay
   green: that is the regression check for the bump on its own.

2. **A process layer without a shell.** Replace the string-then-shell pattern with an argv:
   - a `Command` type (vector<string> argv, optional stdin/stdout/stderr redirection) and one
     `run(const Command&)` built on `boost::process::child(exe, bp::args(argv))` -- direct
     exec, no `/bin/sh -c`, no `cmd.exe`; `bp::search_path()` for a bare program name;
   - `run_command()`, `run_command_quiet()`, the parallel compile worker (l.2606) and the two
     `popen` probes move onto it; `shell_quote()` survives only for the human-readable
     command echo in verbose mode;
   - `/dev/null` becomes `bp::null` on the process side; where a file name is needed on a
     command line (the probes' `-x c++ /dev/null`), use an empty temporary `.cpp` on every
     platform rather than branching on `NUL`.
   This is the bulk of the diff (the ~46 `shell_quote` sites) and it is platform-neutral:
   Linux and macOS run it first, and the byte-for-byte snapshot in
   `launcher.remote_split_on_rbe` plus the 31 fixtures are the check that nothing changed.

3. **The remaining POSIX calls.**
   - `<unistd.h>` → `<io.h>`/`<process.h>` under `_WIN32`, or drop: `access(X_OK)` becomes a
     `fs::status` permission check, `getpid()` gets `_getpid()`;
   - `/proc/self/exe` → `boost::dll::program_location()` (Boost.DLL is header-only for this
     call; add `dll` to `BOOST_INCLUDE_LIBRARIES`) or `GetModuleFileNameW` under `_WIN32`,
     `_NSGetExecutablePath` on macOS -- the macOS path is wrong today too, it just is not
     reached because the remote split skips there.

4. **Paths.** `fs::path::string()` yields backslashes on Windows, and the splitter writes
   paths into `#line` directives, `#include "..."` lines, depfiles and the `.harvest`/`.keeps`
   caches. Use `generic_string()` for everything that lands in a source file or a depfile
   (forward slashes are valid in all of them on Windows), keep native for `execute`. The
   depfile rewrite (TODO/28) has to accept both spellings on read. The include-dir probe
   returns paths as clang prints them (forward slashes under MSYS2's clang); normalise once at
   the boundary.

5. **Output names.** The launcher already takes `-o` as given. The CLI mode's `--compile -o
   program` and the test drivers get `${CMAKE_EXECUTABLE_SUFFIX}` on the program name and
   nothing on objects (`.o` is fine for MinGW). The drivers additionally pass the MSYS2 `ld`
   explicitly through `CPP_SPLITTER_LINKER` only if `ld` is not first on PATH in the job.

6. **CMakeLists.txt.** `-stdlib=libc++` only where the compiler accepts it (MSYS2 CLANG64's
   clang uses libc++ by default, but do not pass the flag to a MinGW gcc); link
   `libclang.dll.a` from `CPP_SPLITTER_LIBCLANG_ROOT/lib` (`CMAKE_IMPORT_LIBRARY_SUFFIX`
   rather than `CMAKE_SHARED_LIBRARY_SUFFIX` on Windows) and no `BUILD_RPATH` -- the DLL is
   found through PATH, which the MSYS2 shell provides. The Linux branch keeps its exact link
   line as today.

7. **`environments/windows-msys2-clang.cmake`.** A host toolchain naming
   `C:/msys64/clang64/bin/clang++.exe` (what `msys2/setup-msys2` installs on the runner) and
   `CPP_SPLITTER_LIBCLANG_ROOT` = `C:/msys64/clang64`, C++17 pinned like the others, host
   check for `Windows`. Plain cmake, not cmake-re: cmake-re's Windows support is for the
   `vs-*` toolchains and would need its own validation; this job is the reference build the
   way `plain-cmake-build-and-test` is on Linux.

8. **The CI job** `windows-msys2-clang-build-and-test` on `windows-latest`:
   `msys2/setup-msys2@v2` with `msystem: CLANG64` and packages
   `mingw-w64-clang-x86_64-{clang,clang-tools-extra,cmake,ninja,lld}` (`clang-libs` comes
   with clang); `shell: msys2 {0}` for every step; configure with the toolchain of step 7;
   `cmake --build`; `ctest --output-on-failure`. Cache `thirdparty/cache` like the plain job.
   Upload `cpp-splitter.exe` as `cpp-splitter-windows-x86_64` and add it to the draft release,
   with the note that it needs the MSYS2 CLANG64 clang on PATH.

9. **Docs.** README gets a folded Windows section; DOCS.md's "Relocatable Linking" paragraph
   gains the sentence that on Windows the linker is binutils/lld `ld -r` over COFF and that
   MSVC is out of scope, with the reason from the Decision above.

Out of scope, deliberately: cmake-re on Windows, an MSVC-driven split, `--distributed` from
Windows (reclient's Windows support is a separate question), and the Boost benchmarks.

## Acceptance Criteria

- Steps 1-4 land first with no Windows job, and the three existing jobs stay green with the
  same 33/33; `launcher.remote_split_on_rbe`'s local-vs-remote snapshot still matches once it
  runs (needs cmake-re v0.0.88, see TODO/37).
- `windows-msys2-clang-build-and-test` builds `cpp-splitter.exe` and passes the 31 split and
  launcher fixtures (`launcher.remote_split_on_rbe` skips: not Linux).
- `split.use_mylib` passes on Windows -- it is the fixture that exercises a quoted sibling
  header through the mirror, where path spelling (step 4) goes wrong first.
- `launcher.depfile_names_originals` and `launcher.incremental_body_edit` pass on Windows:
  the depfile rewrite and the harvest cache are where backslashes would otherwise surface.
- The draft release carries a third asset, `cpp-splitter-windows-x86_64.exe`.
- `example/boost-to-split`: not part of this entry; the Boost harness needs the vendored
  checkout and is Linux-only by its scripts.

## Open questions, to settle before step 2

- Whether to keep `std::system` for the single-shot commands and only move the parallel
  worker and the probes to argv spawning. Cheaper, but leaves two quoting dialects in the
  binary; the proposal above says no.
- Whether `ld.lld -r` (MSYS2 `lld`) or binutils `ld -r` is the default linker on Windows.
  binutils matches Linux; lld is what the CLANG64 environment ships by default. Measure both
  on the fixtures and pick the one that links every fixture; the other stays reachable through
  `CPP_SPLITTER_LINKER`.
