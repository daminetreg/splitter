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

| Scenario | Monolithic | cpp-splitter (server+PCH) | Speedup |
|---|---|---|---|
| Cold build (from scratch) | 19.1s | 36.5s | 0.5x (slower) |
| Rebuild after editing 1 function | 19.1s | 3.7s | **5.1x faster** |
| Rebuild with no changes | 19.1s | 2.4s | **8.0x faster** |

The first row shows the trade-off: a cold split build (including PCH generation, splitting, and compiling all 17 files) is slower than monolithic. This is a one-time cost.

The second row is where it pays off. Monolithic always takes 19.1 seconds regardless of what changed. With cpp-splitter, editing one function takes **3.7 seconds**: the server returns the cached AST in ~0.1s, the PCH is already built, one file compiles in ~1.2s (with PCH skipping header parsing), and the rest is linking. That's a **5.1x speedup** — 15.4 seconds saved on every edit.

The third row shows the best case: nothing changed, so cpp-splitter confirms all `.o` files are up-to-date in 2.4 seconds. The monolithic compiler has no such shortcut — it always does the full 19.1 seconds.

### Where the Time Goes

Breaking down the 3.7-second rebuild after editing one function:

| Phase | Time | What happens |
|---|---|---|
| Splitting | ~0.1s | Server returns cached AST, writes split files |
| PCH check | ~0s | Preamble unchanged, `.gch` is current |
| Compile 1 file | ~1.2s | g++ compiles one function body (PCH skips header parsing) |
| Skip 16 files | ~0s | All other `.o` files are up-to-date |
| Link | ~2.4s | g++ links 17 object files into the binary |

The phases overlap slightly and include process startup overhead, so they don't sum exactly to 3.7s — but the dominant costs are clear: one compilation and one link.

Compare to monolithic, where all 19.1 seconds are a single indivisible `g++` invocation: ~11 seconds parsing Boost.Spirit headers, ~8 seconds generating code for all 17 functions.

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
