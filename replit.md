# C++ Function Splitter

## Overview
A command-line tool that parses a C++ source file using the libclang AST and generates one output file per function/method implementation body. Supports compiling and linking the split files back into a working binary.

## How to Use
```
./cpp-splitter <input.cpp> [output_dir] [options] [-- <clang_flags>...]
```
- `input.cpp` - the C++ source file to split
- `output_dir` - directory for output files (default: `./output`)

### Options
- `--compile` - compile and link the split files into a binary
- `-o <binary>` - output binary name (default: `<stem>.out`)
- `--cxx <compiler>` - C++ compiler to use (default: `g++`)
- `-- <flags>` - extra flags passed to clang parser (e.g., `-I/path/to/include`)

### Examples
```
./cpp-splitter src/app.cpp output                          # split only
./cpp-splitter src/app.cpp output --compile -o myapp       # split + compile + link
./cpp-splitter src/app.cpp output --compile -- -std=c++20  # with extra clang flags
```

## Project Architecture
```
src/main.cpp       - Main tool source code (uses libclang C API)
Makefile           - Build system (g++ with libclang linking)
test/sample.cpp    - Sample C++ file for testing
```

## Build
```
make          # builds ./cpp-splitter
make clean    # removes binary
```

## Dependencies
- C++ compiler (g++)
- libclang (clang-19.1.7 from Nix)
- C++17 standard

## Key Decisions
- Uses the libclang C API (`clang-c/Index.h`) for AST parsing
- Handles: free functions, class methods, constructors, destructors, function templates
- Recurses into namespaces, classes, structs, and class templates
- Output files include comments with function signature, source file, and line range
- Generates a preamble header with declarations for compilable split files
- Template functions are kept in the preamble header (header-only)
- Static functions are split normally but renamed via direct text replacement to avoid linker collisions
  - Name pattern: `__static_<filestem>__<funcname>` (e.g., `__static_sample__helper_function`)
- Forward declarations for namespace/free functions are auto-generated
- Namespace-scoped functions are wrapped in proper namespace blocks in split files
- `#line` preprocessor directives in both preamble and split files map compiler errors/debug info back to original source locations
  - Preamble: `#line 1 "original.cpp"` at top, re-syncs after each skipped function body
  - Split files: `#line <start_line> "original.cpp"` before each function body
  - Line offset table built via `build_line_offsets()` for efficient offset-to-line conversion

## Recent Changes
- 2026-02-07: Preprocessor location maps — `#line` directives in preamble and split files map to original source
- 2026-02-07: Incremental re-splitting — only writes files that changed or are missing, removes stale files
- 2026-02-07: Static functions now split with unique mangled names per source file (direct renaming, no macros)
- 2026-02-07: Added --compile flag for compiling and linking split files
- 2026-02-07: Initial implementation
