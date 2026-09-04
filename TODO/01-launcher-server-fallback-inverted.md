# 01 — Launcher mode never splits when no server is running

**Severity:** Blocker. Nothing else in this list is observable until this is fixed.

## Motivation

`cpp-splitter` used as `CMAKE_CXX_COMPILER_LAUNCHER` silently does nothing unless a
`--server` process happens to be running. It falls straight through to the original
compiler command, the build succeeds, the objects are byte-identical to a normal build,
and no `.split/` directories are produced. There is no warning.

Observed on a minimal Boost 1.85 `filesystem` build: the launcher build finished in
0.686s — exactly the baseline time — with zero split output. The tool appeared to work
and did nothing at all. Anyone evaluating the splitter through CMake will conclude it is
a no-op.

`DOCS.md` already claims this was fixed ("Fixed launcher mode to fall back to local
`do_split()` when server is unavailable"). It is not fixed; the documentation should be
corrected along with the code.

## Description

In `run_as_launcher()` (`src/main.cpp`, ~line 1820) the two branches are inverted
relative to the message they print:

```cpp
const char* no_server = std::getenv("CPP_SPLITTER_NO_SERVER");
if (!no_server || std::string(no_server) != "1") {
    sr = try_server_split(input_file, split_dir, split_flags, verbose, std::cout);
} else {
    std::cerr << "[cpp-splitter] server unavailable, splitting locally\n";
    sr = do_split(input_file, split_dir, split_flags, verbose);
}
```

The local `do_split()` path runs **only** when the user explicitly sets
`CPP_SPLITTER_NO_SERVER=1` — that is, only when they asked for the server *not* to be
used. When the server is genuinely unavailable, `try_server_split()` returns
`success == false` and control drops to the `if (!sr.success)` passthrough below, which
runs the untouched compiler command.

CLI mode gets this right (`src/main.cpp:2113-2123`):

```cpp
if (!sr.success) {
    sr = do_split(input_path, output_dir, extra_flags, true);
}
```

### Implementation plan

1. Restructure the block in `run_as_launcher()` to mirror CLI mode:
   - If `CPP_SPLITTER_NO_SERVER=1`, skip the server entirely and call `do_split()`.
   - Otherwise call `try_server_split()`; if it fails, log
     `"[cpp-splitter] server unavailable, splitting locally"` and fall back to
     `do_split()`.
   - Only if *that* also fails should the passthrough path run.
2. Factor the shared "server, else local" selection out of `run_as_launcher()` and
   `main()` into one helper (e.g. `split_via_server_or_locally(...)`) so the two modes
   cannot drift apart again.
3. Distinguish "server unreachable" from "server reached, split failed" in
   `try_server_split()`'s result. Retrying locally is right for the former; for the
   latter it will usually fail the same way, and a duplicated failure is worth logging
   differently.
4. Emit a one-line notice on the passthrough path even when `CPP_SPLITTER_VERBOSE` is
   off, so a build that silently degrades to plain compilation is visible. Gate it
   behind a `CPP_SPLITTER_QUIET_FALLBACK=1` opt-out for people who want clean logs.
5. Update the "Recent Changes" entry in `DOCS.md` that claims this already works.

## Acceptance Criteria

- With no server running and no `CPP_SPLITTER_*` environment variables set, a launcher
  build of the Boost `filesystem` example produces `.split/` directories and split
  `.cpp` files.
- `CPP_SPLITTER_NO_SERVER=1` still forces the local path and never contacts the socket.
- With a server running, the server path is still used (verify via
  `CPP_SPLITTER_VERBOSE=1` output).
- A build that falls back to plain compilation says so on stderr without requiring
  verbose mode.
- Regression test: run the launcher against a trivial `.cpp` with the socket path
  pointed at a non-existent file and assert that split output is produced.

## Reproduction

```sh
tipi run cmake -GNinja -S example/boost-to-split -B build-split \
  -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DBOOST_INCLUDE_LIBRARIES=filesystem \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
tipi run cmake --build build-split -j8
find build-split -name '*.split' | wc -l    # 0 before the fix
```
