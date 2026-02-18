# 5x Faster C++ Rebuilds: Splitting Boost.Spirit Files by Function

## The Problem: 19 Seconds to Change One Line

Boost.Spirit is one of C++'s most powerful parsing libraries — and one of its most expensive to compile. A single 510-line source file using Spirit's `qi`, `karma`, `phoenix`, and `fusion` headers takes **19 seconds** to build with g++ at `-O0`. That's 19 seconds of developer time burned every time you touch a single function, even though 16 of the 17 functions in the file haven't changed.

The compiler doesn't know that. It parses every header, re-instantiates every template, and regenerates code for every function — all because the build system tracks dependencies at the file level, not the function level.

## The Idea: One File Per Function

What if we split the source file into 17 separate compilation units — one per function — and only recompiled the one that changed?

`cpp-splitter` does exactly this. It uses libclang's AST to parse a C++ source file, extract each function definition, and emit it as a standalone `.cpp` file. A shared preamble header contains everything the functions need: includes, type definitions, class declarations, and forward declarations.

```
$ ./cpp-splitter spirit_example.cpp output

Found 17 function(s) in spirit_example.cpp:

  [1] bool parse_csv_line(const std::string &, std::vector<std::string> &)
      Lines 32-41 -> output/spirit_example_1_parse_csv_line.cpp
  [2] bool parse_integer_list(const std::string &, std::vector<int> &)
      Lines 43-52 -> output/spirit_example_2_parse_integer_list.cpp
  ...
  [17] int main()
      Lines 492-510 -> output/spirit_example_17_main.cpp
```

Each split file includes the preamble and contains exactly one function body with a `#line` directive pointing back to the original source, so compiler errors and debugger line numbers still reference the right location.

## Three Optimizations That Stack

Splitting alone isn't enough for heavy headers like Boost.Spirit. Each split file would independently re-parse the same expensive headers, making the build *slower* than monolithic. Three optimizations work together to make it fast:

### 1. Precompiled Headers (PCH)

The preamble header — which contains all the `#include` directives — is precompiled once into a `.gch` file. g++ automatically picks it up, so each split file skips header parsing entirely. For Boost.Spirit, this takes per-file compilation from ~10 seconds down to ~1.2 seconds.

### 2. Persistent Server Mode

Splitting requires libclang to parse the source file and build an AST. For Boost.Spirit, this takes **10.7 seconds** — and it happens fresh on every invocation because the process exits and throws away the parsed state.

The server mode keeps a background process alive that holds parsed translation units in memory:

```bash
$ ./cpp-splitter --server &
[cpp-splitter server] listening on /tmp/cpp-splitter-1000.sock
```

Every subsequent `cpp-splitter` invocation connects to the server automatically. If the source file hasn't changed, the server returns the cached AST in **105 milliseconds** — a 102x speedup. If the source has been edited, it calls `clang_reparseTranslationUnit()` which reuses the precompiled preamble (all the Boost template headers stay cached) and re-parses only the source body — **2.9 seconds** instead of 10.7.

### 3. Incremental Recompilation

Only modified split files get recompiled. Change one function out of 17? One file is compiled, 16 are skipped. The PCH and all other `.o` files remain valid.

## Benchmark: Monolithic vs Split+Server+PCH

Boost.Spirit parser example (510 lines, 17 functions). g++ 14.3.0, `-O0`, 6-core machine. "cpp-splitter" means server mode with warm TU cache and precompiled headers — the steady-state configuration during development.

The total build time has two distinct phases: **splitting** (parsing the source, walking the AST, writing split files) and **compiling + linking** (g++ compiles each split file, then links the objects into a binary). These phases have very different performance characteristics and optimization strategies.

### Splitting Phase

Splitting is the work that cpp-splitter does before the compiler is even invoked: parse the source with libclang, identify function boundaries, and write out the split `.cpp` files and preamble header.

| Scenario | Time | Notes |
|---|---|---|
| Local parse (no server) | 10.7s | Full libclang parse of source + Boost.Spirit headers |
| Server cold (first parse) | 10.8s | Same as local — TU cache is empty |
| Server warm (cached TU) | 0.1s | Cached AST, no parsing — **102x faster** |
| Server reparse (source edited) | 2.9s | `clang_reparseTranslationUnit()` reuses preamble — **3.7x faster** |

Without the server, splitting alone takes 10.7 seconds — more than half the monolithic build time — just to parse Boost.Spirit's template-heavy headers and walk the AST. The server eliminates this entirely on warm cache (0.1s), and even when the source has been edited, reparsing reuses the precompiled preamble and only re-parses the source body (2.9s).

In a real-world project, the splitting results can also be cached externally. A remote build cache like [cmake-re](https://cmake-re.com) can store the split file outputs keyed by source content hash and compiler flags. Once a file has been split on any machine, subsequent builds (on the same or different machines) skip splitting entirely and pull the cached split files. This means the splitting cost — even the 10.7s cold parse — is paid at most once across your entire team.

### Compile + Link Phase

Once the split files exist, g++ compiles and links them. This is where PCH and incremental recompilation pay off.

| Scenario | Time | Notes |
|---|---|---|
| Cold compile (all 17 files + PCH build) | ~25.7s | Build PCH once (~10s), then compile 17 files in parallel (~1.2s each) + link |
| 1-function recompile + link | ~3.6s | PCH cached, compile 1 file (~1.2s), skip 16 files, link (~2.4s) |
| No-change rebuild | ~2.3s | PCH cached, all `.o` files up-to-date, only timestamp checks + link |

Without PCH, each split file would independently parse Boost.Spirit headers (~10s per file). With PCH, per-file compilation drops to ~1.2s — an **8.5x per-file speedup**. The PCH is rebuilt only when the preamble changes (new includes, changed struct definitions), so it adds zero cost to typical function edits.

### Total: Splitting + Compile + Link

Combining both phases gives the end-to-end comparison against monolithic:

| Scenario | Monolithic | cpp-splitter (server+PCH) | Speedup |
|---|---|---|---|
| Cold build (from scratch) | 19.1s | 36.5s | 0.5x (slower) |
| Rebuild after editing 1 function | 19.1s | **3.7s** (0.1s split + 3.6s compile/link) | **5.1x faster** |
| Rebuild with no changes | 19.1s | **2.4s** (0.1s split + 2.3s compile/link) | **8.0x faster** |

The cold build is slower because it pays the one-time costs: full parse, PCH generation, and compiling all 17 files. Every subsequent rebuild benefits from all three caches (server TU cache, PCH, incremental `.o` files).

The key insight is that splitting becomes negligible (~0.1s) once the server is warm, so the rebuild time is dominated by compile + link. And with PCH + incremental recompilation, compile + link is dominated by just the one changed file plus the final link step.

Compare to monolithic, where all 19.1 seconds are a single indivisible `g++` invocation: ~11 seconds parsing Boost.Spirit headers, ~8 seconds generating code for all 17 functions. There is no way to skip any of it — even if nothing changed.

### Over a Development Session

In practice, the savings compound. A developer editing a Spirit-heavy file might rebuild 30 times in an afternoon:

| | Monolithic | cpp-splitter + server |
|---|---|---|
| 30 rebuilds | 9 min 33s | 1 min 51s |
| Time saved | — | **7 min 42s** |

That's nearly 8 minutes saved in a single afternoon, on a single file. Across a project with multiple heavy `.cpp` files, the savings scale proportionally.

## How It Works

### AST-Based Splitting

cpp-splitter uses the libclang C API to walk the translation unit's AST. It handles free functions, class methods, constructors, destructors, namespace-scoped functions, and function templates. Template functions are kept in the preamble header (they can't be split into separate translation units).

The tool extracts forward declarations directly from the source text — the exact characters before each function's opening `{`. This is more reliable than using libclang's type resolution, which can produce incorrect signatures when types aren't fully resolved.

### Static Function Renaming

Static functions have internal linkage. When split into separate translation units, they'd cause linker conflicts. cpp-splitter renames them with a unique mangled name (`__static_<filestem>__<funcname>`) and applies the same renaming at all call sites.

### Server Architecture

The server is intentionally simple: single-threaded, Unix domain socket, no authentication. It maintains a map of source file paths to cached `CXTranslationUnit` objects. Cache invalidation is timestamp-based:

- **Source mtime unchanged**: Return cached TU directly
- **Source mtime changed**: Call `clang_reparseTranslationUnit()` (reuses precompiled preamble, re-parses only the source body)
- **Compiler flags changed**: Discard cached TU, do a full parse

Compilation always happens client-side. The server only accelerates the parsing and splitting phase.

### CMake Integration

cpp-splitter can act as a `CMAKE_CXX_COMPILER_LAUNCHER`, transparently intercepting compilation commands:

```bash
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/cpp-splitter ..
make -j$(nproc)
```

No CMakeLists.txt changes needed. The tool splits each source file, compiles the pieces, and combines them via `ld -r` into the single `.o` file the build system expects.

## Limitations

- **Cold build overhead**: The first build is slower (splitting + PCH build + parallel compile). The tool is optimized for the edit-compile-test cycle, not CI/CD clean builds.
- **Template-heavy code**: Function templates must stay in the header. Files that are mostly templates see less benefit from splitting.
- **Header changes**: Modifying an included header invalidates all split files and the PCH — same as monolithic. The savings come from source file edits.
- **Link time**: More `.o` files mean slightly more linker work, though `ld -r` in launcher mode mitigates this.

## Conclusion

For Boost.Spirit and similarly header-heavy C++ code, cpp-splitter with server mode turns a 19-second monolithic rebuild into a 3.7-second incremental one. The three optimizations — persistent TU cache, precompiled headers, and per-function incremental compilation — eliminate the three sources of redundant work: re-parsing the source AST, re-parsing headers in each compilation unit, and re-compiling unchanged functions.

Start the server, edit a function, rebuild in under 4 seconds. Over a day of development, that's minutes of saved waiting — on a single file.
