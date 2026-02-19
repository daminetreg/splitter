# C++ Function Splitter

## Overview
The C++ Function Splitter is a command-line tool designed to parse C++ source and header files using the libclang AST. Its primary purpose is to split C++ function and method implementations into individual output files. It supports compiling and linking these split files back into a working binary. The tool can also split inline functions from header files, automatically handling include path substitutions and object file linking. Furthermore, it can integrate with CMake as a `CMAKE_CXX_COMPILER_LAUNCHER` to transparently split and compile C++ code during the build process, leveraging a persistent server for efficient caching of translation units.

## User Preferences
I want iterative development. Ask before making major changes. I prefer detailed explanations for complex architectural decisions.

## System Architecture
The C++ Function Splitter is implemented in `src/main.cpp` and uses the libclang C API for AST parsing. It employs `CXTranslationUnit_PrecompiledPreamble` and `CXTranslationUnit_CreatePreambleOnFirstParse` flags for efficient internal preamble caching, avoiding redundant header parsing.

**Core Features:**
- **Function Splitting:** Parses C++ source to extract and split free functions, class methods, constructors, destructors, and function templates. It recurses into namespaces, classes, structs, and class templates.
- **Header Splitting:** Extracts inline functions from header files (.h/.hpp/.hxx) into separate .cpp files, keeping the `inline` keyword on bodies and stripping it from forward declarations. Uses `-fkeep-inline-functions` to force symbol emission. Template functions remain in headers (header-only). It uses manifest files (`.split`) to track and resolve dependencies for split headers.
- **Automatic Header Detection:** When processing a source file, uses `clang_getInclusions()` to detect all `#include`d headers. Project and `-isystem` headers are automatically split; C++ standard library headers (detected via `g++ -E -v`) are skipped. Staleness is tracked via manifest timestamps.
- **Source-Text Signature Extraction:** Function signatures are extracted directly from source text (before the opening `{` of the function body) for reliability, handling `static` and `inline` keyword stripping and namespace prefix removal.
- **Relocatable Linking:** In compiler launcher mode, split files are compiled separately and then combined into a single `.o` file using relocatable linking (`ld -r`).
- **Preamble Generation:** A preamble header with declarations for compilable split files is generated.
- **Line Directives:** `#line` preprocessor directives are used in both the preamble and split files to map compiler errors and debug information back to original source locations.
- **Static Function Handling:** Static functions are split and renamed with a unique mangled name (e.g., `__static_<filestem>__<funcname>`) to prevent linker collisions.
- **Namespace Handling:** Namespace-scoped functions are properly wrapped within their respective namespace blocks in the split files.
- **Precompiled Headers (PCH):** Automatically builds PCH for the preamble header before compiling split files, significantly speeding up compilation for heavy headers.
- **Incremental Recompilation:** Compares timestamps of source files, preamble, and PCH against object files to recompile only changed components.
- **Parallel Compilation:** Utilizes Boost.Process and `std::thread` for concurrent compilation of split files using a work-stealing pattern.
- **Compiler Launcher Mode:** Operates as a compiler wrapper, splitting the source via a server, compiling each piece, and combining them into a single `.o`. It handles dependency tracking flags (`-MD`/`-MMD`/`-MF`/`-MT`).
- **Server Mode:** A persistent background server uses a Unix domain socket to maintain a cache of parsed translation units. This allows subsequent split requests to reuse cached preambles, accelerating parsing. The server employs `clang_reparseTranslationUnit()` for efficient updates when only source body changes.

**UI/UX and Design Patterns:**
- Comments are added to output files with function signatures, source file, and line range for traceability.
- The tool auto-detects C++ compiler system include paths by executing `g++ -E -x c++ /dev/null -v` to ensure correct resolution of standard library types.

## External Dependencies
- **C++ compiler:** g++
- **libclang:** Specifically `clang-19.1.7` (from Nix in the original context).
- **Boost:** Version 1.87.0, primarily Boost.Process for parallel compilation.
- **ld:** For relocatable linking, especially in compiler launcher mode.