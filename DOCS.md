# C++ Function Splitter

## Overview
The C++ Function Splitter is a command-line tool designed to parse C++ source and header files using the libclang AST. Its primary purpose is to split C++ function and method implementations into individual output files. It supports compiling and linking these split files back into a working binary. The tool can also split inline functions from header files, automatically handling include path substitutions and object file linking. Furthermore, it can integrate with CMake as a `CMAKE_CXX_COMPILER_LAUNCHER` to transparently split and compile C++ code during the build process.

## User Preferences
I want iterative development. Ask before making major changes. I prefer detailed explanations for complex architectural decisions.

## System Architecture
The C++ Function Splitter is implemented in `src/main.cpp` and uses the libclang C API for AST parsing. It employs `CXTranslationUnit_PrecompiledPreamble` and `CXTranslationUnit_CreatePreambleOnFirstParse` flags for efficient internal preamble caching, avoiding redundant header parsing.

**Core Features:**
- **Function Splitting:** Parses C++ source to extract and split free functions, class methods, constructors, destructors, and function templates. It recurses into namespaces, classes, structs, and class templates.
- **Header Splitting:** Extracts inline functions from header files (.h/.hpp/.hxx) into separate .cpp files, keeping the `inline` keyword on bodies and stripping it from forward declarations. Template functions remain in headers (header-only). It uses manifest files (`.split`) to track and resolve dependencies for split headers.
- **Automatic Header Detection:** When processing a source file, uses `clang_getInclusions()` to detect all `#include`d headers. Project and `-isystem` headers are automatically split; C++ standard library headers (detected via `g++ -E -v`) are skipped. Staleness is tracked via manifest timestamps.
- **Linkage of split definitions:** A definition taken out of a `.cpp` is the only one that will exist, so `inline` is stripped from it and it becomes an ordinary strong symbol; the declaration left in the preamble matches. A definition taken out of a *header* keeps `inline`, because the header may be included by many translation units and each gets its own split copy -- vague linkage is what lets the linker merge them into exactly one definition. Since an inline function that its own translation unit never odr-uses is not emitted at all, header-split definitions are marked `__attribute__((used))` to force the symbol into existence. This replaces `-fkeep-inline-functions`, which is a GCC option that clang parses and ignores, so it never forced anything under this project's toolchain. The trade-off of stripping `inline` from `.cpp` definitions is that such functions can no longer be inlined into their callers, which is inherent to splitting them into separate objects.
- **Source-Text Signature Extraction:** Function signatures are extracted directly from source text (before the opening `{` of the function body) for reliability, handling `static` and `inline` keyword stripping and namespace prefix removal.
- **Relocatable Linking:** In compiler launcher mode, split files are compiled separately and then combined into a single `.o` file using relocatable linking. `CPP_SPLITTER_LINKER` names the linker, defaulting to `ld`; it is passed `-r` either way, so `mold` and `ld.lld` work as drop-in alternatives. mold is about twice as fast at this step and produces a smaller object, though the step is only a few percent of a split build.
- **Preamble Generation:** A preamble header with declarations for compilable split files is generated.
- **Line Directives:** `#line` preprocessor directives are used in both the preamble and split files to map compiler errors and debug information back to original source locations.
- **Static Function Handling:** Static functions are split and renamed with a unique mangled name (e.g., `__static_<filestem>__<funcname>`) to prevent linker collisions.
- **Namespace Handling:** Namespace-scoped functions are properly wrapped within their respective namespace blocks in the split files.
- **Precompiled Headers (PCH):** Two-level PCH system:
  - *libclang PCH* (`<preamble>.pch/<hash>.pch`): Built via `clang_saveTranslationUnit()` from the preamble header. On subsequent runs, passed as `-include-pch` to `clang_parseTranslationUnit2()` to dramatically speed up AST parsing (replacing `CXTranslationUnit_PrecompiledPreamble`). This artefact currently has no consumer; see `TODO/08` and `TODO/14`.
  - *GCC PCH* (`<preamble>.gch/<hash>.gch`): Built for compilation of split files. GCC auto-discovers PCH via the `.gch/` directory.
  - Both use content-hash naming: if the hash-named file exists, the PCH is current; otherwise old entries are cleaned and it rebuilds.
- **Incremental Recompilation:** Compares timestamps of source files, preamble, and PCH against object files to recompile only changed components.
- **Parallel Compilation:** Utilizes Boost.Process and `std::thread` for concurrent compilation of split files using a work-stealing pattern.
- **Compiler Launcher Mode:** Operates as a compiler wrapper, splitting the source, compiling each piece, and combining them into a single `.o`. It handles dependency tracking flags (`-MD`/`-MMD`/`-MF`/`-MT`).

**UI/UX and Design Patterns:**
- Comments are added to output files with function signatures, source file, and line range for traceability.
- The tool auto-detects C++ compiler system include paths by executing `g++ -E -x c++ /dev/null -v` to ensure correct resolution of standard library types.

**Verbose/Debug Logging:**
- Set `CPP_SPLITTER_VERBOSE=1` (or `=on`) to enable verbose logging in launcher mode.
- Launcher logs: shows input file, split flags, compile commands, dependency-file rewriting, and `ld -r` invocations.

**Recent Changes:**
- Removed the split server entirely: the socket protocol, the in-memory translation unit
  cache, `--server`/`--socket`, and `CPP_SPLITTER_SOCKET`/`CPP_SPLITTER_NO_SERVER`. The
  launcher splits locally and needs no environment variable to do so. Caching that survives
  a distributed build is proposed in `TODO/14` instead.
- The launcher rewrites the dependency file so it names the original headers rather than the
  rewritten copies, and caches it so a build that recompiles nothing still reports what it
  knows. Without this, editing a header rebuilt nothing that included it.
- The preamble is layered by linkage: definitions that may appear in many objects stay in it,
  and those that may appear once go to a definitions header included by exactly one piece.
- Only functions the translation unit actually emits are split, computed by reachability from
  the definitions the compiler must emit.

## External Dependencies
- **C++ compiler:** g++
- **libclang:** Specifically `clang-19.1.7` (from Nix in the original context).
- **Boost:** Version 1.87.0, primarily Boost.Process for parallel compilation.
- **ld:** For relocatable linking, especially in compiler launcher mode.