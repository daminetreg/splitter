# C++ Function Splitter

## Overview
A command-line tool that parses a C++ source file using the libclang AST and generates one output file per function/method implementation body.

## How to Use
```
./cpp-splitter <input.cpp> [output_dir]
```
- `input.cpp` - the C++ source file to split
- `output_dir` - directory for output files (default: `./output`)

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

## Recent Changes
- 2026-02-07: Initial implementation
