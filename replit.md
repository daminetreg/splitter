# C++ Function Splitter

## Overview
A command-line tool that parses a C++ source file using the libclang AST and generates one output file per function/method implementation body. Supports compiling and linking the split files back into a working binary. Can also be used as a CMAKE_CXX_COMPILER_LAUNCHER to transparently split and compile during CMake builds.

## How to Use

### Mode 1: Direct Split (and optionally compile)
```
./cpp-splitter <input.cpp> [output_dir] [options] [-- <clang_flags>...]
```
- `input.cpp` - the C++ source file to split
- `output_dir` - directory for output files (default: `./output`)

#### Options
- `--compile` - compile and link the split files into a binary
- `-o <binary>` - output binary name (default: `<stem>.out`)
- `--cxx <compiler>` - C++ compiler to use (default: `g++`)
- `-- <flags>` - extra flags passed to clang parser (e.g., `-I/path/to/include`)

#### Examples
```
./cpp-splitter src/app.cpp output                          # split only
./cpp-splitter src/app.cpp output --compile -o myapp       # split + compile + link
./cpp-splitter src/app.cpp output --compile -- -std=c++20  # with extra clang flags
```

### Mode 2: Compiler Launcher (for CMake integration)
When the first argument is not a source file, the tool acts as a compiler wrapper:
```
./cpp-splitter <compiler> [compiler_flags...] -c -o <output.o> <source.cpp>
```

The tool splits the source via the server, compiles each piece separately, and combines them into a single `.o` using relocatable linking (`ld -r`). Non-compilation commands are passed through transparently.

**The server must be running before using launcher mode.** Launcher mode always connects to the server for splitting; there is no fallback to local parsing. This ensures fast splitting via cached translation units during builds.

#### CMake usage
```
./cpp-splitter --server &
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/cpp-splitter ..
```

#### Features
- Transparent wrapper: non-compilation commands pass through directly
- Dependency tracking: `-MD`/`-MMD`/`-MF`/`-MT` flags handled correctly
- Split files stored in `<output>.split/` directory next to the build artifact
- Single `.o` output via `ld -r` relocatable linking (or direct copy for single-function files)
- Verbose mode: set `TIPI_CPP_SPLITTER_VERBOSE=on` to see splitting, compilation, and linking details on stderr

### Mode 3: Server (persistent TU cache)
Starts a background server that keeps parsed translation units in memory, so subsequent split requests reuse the cached preamble instead of re-parsing from scratch.

```
./cpp-splitter --server [--socket <path>]
```

The server listens on a Unix domain socket (default: `/tmp/cpp-splitter-<uid>.sock`). In direct mode (Mode 1), the client tries to connect to the server; if unavailable, it falls back to local parsing. In launcher mode (Mode 2), the server is required — the client will fail with an error if the server is not running.

#### Environment Variables
- `CPP_SPLITTER_SOCKET` - override the Unix socket path
- `CPP_SPLITTER_NO_SERVER=1` - disable client connections to the server

#### Cache Invalidation
- Source file mtime changes → `clang_reparseTranslationUnit()` (reuses precompiled preamble)
- Compiler flags change → full reparse + new cache entry
- Compilation remains client-side; server only accelerates the parsing/splitting phase

#### Examples
```
./cpp-splitter --server &                                 # start server in background
./cpp-splitter src/app.cpp output --compile -o myapp      # uses server if available
CPP_SPLITTER_NO_SERVER=1 ./cpp-splitter src/app.cpp out   # force local parsing
```

## Project Architecture
```
src/main.cpp                    - Main tool source code (uses libclang C API)
Makefile                        - Build system (g++ with libclang linking)
test/sample.cpp                 - Sample C++ file for testing
example/data_processing.cpp     - Data processing example (18 functions)
example/data_processing.h       - Header for data processing example
example/benchmark.sh            - Benchmark script (monolithic vs split compile)
example/benchmark_server.sh     - Server mode benchmark (local vs server parsing + compile)
example/spirit_example.cpp      - Boost.Spirit example (template-heavy, needs lots of RAM)
example/spirit_example.h        - Header for spirit example
```

### Code Structure (src/main.cpp)
- `detect_system_includes()` - auto-detects C++ compiler system include paths for clang parser
- Helper functions: `read_file`, `cx_to_string`, `build_line_offsets`, `sanitize_filename`
- AST visitor: `visitor()` extracts FunctionInfo from clang AST
- `extract_source_signature()` - extracts function signature from source text (not libclang types)
- `generate_forward_decl()` - generates forward declarations from source-text signatures
- Preamble generation: `generate_preamble()` with `#line` directives
- Split file writing: incremental, with `#line` directives
- `build_pch()` - precompiles the preamble header for faster split file compilation
- `compile_parallel()` - parallel compilation using Boost.Process + std::thread
- `build_clang_flags()` - assembles clang parser flags (system includes + extra flags)
- `check_diagnostics()` - checks and reports parse errors from TU
- `do_split()` - core splitting logic (standalone, creates+disposes TU)
- `do_split_with_cache()` - splitting with persistent TU cache (server mode)
- Server infrastructure: `CachedTU`, `g_tu_cache`, Unix socket protocol, `run_server()`
- `try_server_split()` - client: attempts to split via server, falls back gracefully
- `run_as_launcher()` - compiler launcher mode
- `main()` - mode detection and dispatch (direct / launcher / server)

## Build
```
make          # builds ./cpp-splitter
make clean    # removes binary
```

## Dependencies
- C++ compiler (g++)
- libclang (clang-19.1.7 from Nix)
- Boost 1.87.0 (Boost.Process for parallel compilation)
- C++17 standard
- ld (for relocatable linking in launcher mode)

## Key Decisions
- Uses the libclang C API (`clang-c/Index.h`) for AST parsing with `CXTranslationUnit_PrecompiledPreamble | CXTranslationUnit_CreatePreambleOnFirstParse` flags to enable libclang's internal preamble caching (avoids re-parsing headers on repeated invocations)
- Auto-detects C++ system include paths by running `g++ -E -x c++ /dev/null -v` and parsing output
  - Passes detected paths as `-isystem` flags to libclang parser
  - Ensures standard library types (std::string, std::vector, etc.) are properly resolved
- Handles: free functions, class methods, constructors, destructors, function templates
- Recurses into namespaces, classes, structs, and class templates
- Output files include comments with function signature, source file, and line range
- Generates a preamble header with declarations for compilable split files
- Template functions are kept in the preamble header (header-only)
- Forward declarations extracted from source text (not libclang type resolution) for reliability
  - `extract_source_signature()` gets text before opening `{` of function body
  - Handles static keyword stripping and namespace prefix removal
- Static functions are split normally but renamed via direct text replacement to avoid linker collisions
  - Name pattern: `__static_<filestem>__<funcname>` (e.g., `__static_sample__helper_function`)
- Namespace-scoped functions are wrapped in proper namespace blocks in split files
- `#line` preprocessor directives in both preamble and split files map compiler errors/debug info back to original source locations
  - Preamble: `#line 1 "original.cpp"` at top, re-syncs after each skipped function body
  - Split files: `#line <start_line> "original.cpp"` before each function body
  - Line offset table built via `build_line_offsets()` for efficient offset-to-line conversion
- Launcher mode requires a running server — always connects via Unix socket, no fallback to local parsing
- Launcher mode detects source files by extension (.cpp, .cc, .cxx, .C, .c++, .cp, .c)
- Shell quoting via `shell_quote()` for safe command construction
- Automatic precompiled headers (PCH) via `build_pch()`: precompiles the preamble header before split file compilation
  - PCH built with same compiler flags as split files for compatibility
  - g++ auto-detects `.gch` file alongside the included preamble header
  - Incremental: only rebuilds PCH when preamble header is newer than `.gch` file
  - Graceful fallback: if PCH build fails, continues without PCH
  - Works in both direct mode and launcher mode
- Incremental recompilation via `needs_recompile()`: compares .cpp, preamble, and PCH timestamps against .o file
  - Direct mode: shows "(up-to-date)" for skipped files, reports count of skipped vs recompiled
  - Launcher mode: skips up-to-date files, also skips `ld -r` if all objects and output are current
- Single .o files skip `ld -r` and use direct copy for efficiency
- Parallel compilation uses Boost.Process (bp::child) for process spawning and std::thread for worker pool
  - Work-stealing pattern: atomic job counter, threads grab next job until exhausted
  - Thread count = min(num_jobs, hardware_concurrency)
  - stderr captured per-process for clean error reporting
  - In launcher mode with dep flags, first file runs sequentially (generates .d file), rest run in parallel
- Persistent server mode via Unix domain socket for TU cache reuse
  - `CachedTU` struct owns CXIndex + CXTranslationUnit per source file, with RAII cleanup
  - Cache keyed by absolute path; invalidated by mtime change (reparse) or flags change (full reparse)
  - `clang_reparseTranslationUnit()` reuses libclang's precompiled preamble when only source body changes
  - Length-prefixed binary protocol: 4-byte length header + newline-delimited fields
  - Single-threaded server with signal handling (SIGINT/SIGTERM) for socket cleanup
  - Client connection is transparent: `try_server_split()` returns empty SplitResult on failure, caller falls back to local `do_split()`
  - Compilation remains client-side; server only accelerates parsing/splitting
  - Socket path: `/tmp/cpp-splitter-<uid>.sock` (overridable via `CPP_SPLITTER_SOCKET`)

## Recent Changes
- 2026-02-18: Launcher mode now requires server — no fallback to local parsing, ensures fast cached splitting during CMake builds
- 2026-02-18: Persistent server mode — Unix socket server keeps parsed TUs in memory, uses clang_reparseTranslationUnit for preamble reuse across invocations
- 2026-02-18: Automatic precompiled headers (PCH) — precompiles preamble header before split file compilation, 8.5x per-file speedup for heavy headers
- 2026-02-18: Auto-detect C++ system include paths for clang parser — resolves all standard library types correctly
- 2026-02-18: Source-text-based forward declarations — extracts signatures from source text instead of libclang type resolution
- 2026-02-18: Added data_processing example (18 functions, data filtering/sorting/reporting)
- 2026-02-11: Incremental recompilation — only recompile split files whose .cpp or preamble changed (timestamp-based)
- 2026-02-11: Parallel compilation — split files compiled concurrently using Boost.Process + std::thread
- 2026-02-11: Compiler launcher mode — acts as CMAKE_CXX_COMPILER_LAUNCHER, splits + compiles + combines via ld -r
- 2026-02-11: Refactored core splitting into reusable `do_split()` function
- 2026-02-07: Preprocessor location maps — `#line` directives in preamble and split files map to original source
- 2026-02-07: Incremental re-splitting — only writes files that changed or are missing, removes stale files
- 2026-02-07: Static functions now split with unique mangled names per source file (direct renaming, no macros)
- 2026-02-07: Added --compile flag for compiling and linking split files
- 2026-02-07: Initial implementation
