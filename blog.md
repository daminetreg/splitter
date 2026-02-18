# Splitting C++ Source Files for Faster Incremental Builds

## The Problem: One Change, Full Recompile

C++ compilation is notoriously slow. A single `.cpp` file with 20 functions and a heavy header like `<regex>`, `<iostream>`, or Boost.Spirit can take several seconds to compile. Change one function? The compiler rebuilds the entire file from scratch. In large projects, this means waiting seconds (or minutes) every time you touch a single line.

Build systems like CMake track dependencies at the *file* level. If `widget.cpp` contains 30 functions and you fix a typo in one of them, all 30 get recompiled. The compiler parses every header, re-instantiates every template, and regenerates code for functions that haven't changed at all.

What if we could split that one file into 30 separate compilation units — one per function — and only recompile the one that changed?

## cpp-splitter: Per-Function Compilation Units

That's exactly what `cpp-splitter` does. It uses libclang's AST to parse a C++ source file, identify every function/method definition, and emit each one as a separate `.cpp` file that can be independently compiled.

```
$ ./cpp-splitter src/data_processing.cpp output --compile -o data_app -- -Iexample

Found 18 function(s) in data_processing.cpp:

  [1] std::vector<Record> generate_test_data(int)
      Lines 14-40 -> output/data_processing_1_generate_test_data.cpp
  [2] std::vector<Record> parse_csv_records(const std::string &)
      Lines 42-68 -> output/data_processing_2_parse_csv_records.cpp
  ...
  [18] int main()
      Lines 371-374 -> output/data_processing_18_main.cpp

--- Compiling split files ---
  [parallel: 6 threads, 17 jobs]
  ...
Build successful: data_app
```

Each split file includes a shared preamble header containing everything *except* function bodies: includes, type definitions, class declarations, and forward declarations. The preamble plus the individual function body form a complete, compilable translation unit.

## How It Works

### 1. AST-Based Splitting

The tool uses libclang's C API to walk the AST and find function definitions. It handles:

- Free functions and static functions
- Class methods, constructors, destructors
- Namespace-scoped functions
- Function templates (kept in the header, since they must be visible at the point of use)

The AST approach is more reliable than text-based heuristics. It correctly handles nested braces, string literals containing braces, preprocessor conditionals, and other constructs that would trip up a regex-based splitter.

### 2. Auto-Detected System Includes

A common pitfall with libclang: it doesn't automatically know where your compiler's standard library headers live. Without them, types like `std::string` or `std::vector<T>` are unresolved, and functions returning those types are invisible to the AST.

cpp-splitter solves this by running `g++ -E -x c++ /dev/null -v` to discover the compiler's system include paths, then passes them as `-isystem` flags to the clang parser. This ensures full type resolution without requiring the user to manually specify include paths.

### 3. Source-Text Forward Declarations

The preamble needs forward declarations for every split-out function so they can call each other. A naive approach would use libclang's type resolution to generate these, but that fails when types aren't fully resolved (producing wrong signatures like `int foo()` instead of `std::string foo()`).

Instead, cpp-splitter extracts the declaration directly from the source text — the exact characters before the opening `{` of each function body. This always produces the correct signature because it's the literal text the programmer wrote.

### 4. Preprocessor Location Maps

Compiler errors and debugger line numbers need to point back to the original source file, not the split fragments. cpp-splitter inserts `#line` directives in both the preamble and split files:

```cpp
// In split file:
#line 42 "/path/to/data_processing.cpp"
std::vector<Record> parse_csv_records(const std::string& csv_data) {
    // ... original function body ...
}
```

When the compiler reports an error, it shows the original file and line number as if the code was never split.

### 5. Static Function Renaming

Static functions have internal linkage — they're file-scoped and invisible to other translation units. When split into separate files, each becomes its own translation unit, creating linker conflicts if two files define the same static function name.

cpp-splitter renames static functions with a unique mangled name (`__static_<filestem>__<funcname>`) and applies the same renaming at all call sites within the preamble. This preserves the original semantics while avoiding linker collisions.

## Incremental Compilation

The real payoff comes from incremental builds. cpp-splitter tracks timestamps:

- If a split `.cpp` file hasn't changed since its `.o` was compiled, skip it
- If the preamble hasn't changed (no new includes, no struct changes), existing `.o` files stay valid
- Only the modified function(s) get recompiled

For a file with 18 functions, changing one function means recompiling ~6% of the code instead of 100%.

## Parallel Compilation

Split files are compiled in parallel using a thread pool with work-stealing. The thread count matches hardware concurrency (typically the number of CPU cores). Each compilation runs as a separate process via Boost.Process, with stderr captured per-process for clean error reporting.

## Benchmark Results

All benchmarks run on a 6-core machine with g++ 14.3.0 and `-O0` (debug builds, typical of the edit-compile-test cycle).

### Small File: Data Processing Module (374 lines, 18 functions)

Standard library headers (`<algorithm>`, `<regex>`, `<random>`, `<sstream>`, etc.):

| Scenario | Time |
|---|---|
| Monolithic full build | 3,013ms |
| Split + parallel compile (cold) | 11,360ms |
| Incremental rebuild (no changes) | 2,537ms |
| Incremental rebuild (1 function) | 4,067ms |

For this small file, the cold split build is ~3.8x slower — the overhead of spawning 18 compiler processes, each independently parsing the included headers, dominates. The "no changes" case saves 16% by skipping compilation entirely.

### Large File: Boost.Spirit Parser (510 lines, 17 functions)

Template-heavy headers (`boost/spirit/include/qi.hpp`, `karma.hpp`, `phoenix.hpp`, `fusion`):

| Scenario | Time |
|---|---|
| Monolithic full build | 18,216ms |
| Split + parallel compile (cold) | 95,192ms |
| Incremental rebuild (no changes) | 10,786ms |
| Incremental rebuild (1 function) | 23,594ms |

Boost.Spirit is a template-heavy library — the compiler instantiates thousands of template specializations just from the includes, making the monolithic build take **18 seconds**.

The cold split build is 5.2x slower (95s) because each of the 17 split files independently parses those same heavy headers. This is a one-time cost.

The incremental results are more nuanced:

- **No changes**: 10.8s — 41% faster than monolithic. The tool detects all `.o` files are up-to-date and skips compilation entirely. The 10.8s is entirely clang re-parsing the source to confirm nothing changed; no compiler is invoked.
- **1 function changed**: 23.6s — only 1 of 17 files is recompiled, but this single recompilation still takes ~13s because it must re-parse all the Boost.Spirit headers. Combined with the ~10.8s splitting overhead, the total is **slower than monolithic** (23.6s vs 18.2s).

### The Fix: Automatic Precompiled Headers

The bottleneck is clear: each split file independently re-parses the same expensive headers. The solution is to precompile the preamble header once and reuse it across all split files.

cpp-splitter now automatically builds a precompiled header (PCH) from the preamble before compiling split files. The PCH is built with the same flags as the split files, and g++ automatically picks it up when it finds a `.gch` file alongside the included header. The PCH is rebuilt only when the preamble changes (timestamp-based), so it adds zero cost to no-change rebuilds.

### Boost.Spirit with PCH

| Scenario | Without PCH | With PCH | Improvement |
|---|---|---|---|
| Monolithic full build | 18,216ms | 19,843ms | baseline |
| Split + compile (cold) | 95,192ms | 45,523ms | 52% faster |
| No-change rebuild | 10,786ms | 10,662ms | ~same |
| 1-function rebuild | 23,594ms | 11,435ms | 52% faster |

The results are dramatic:

- **Cold build**: Cut in half (95s → 46s). The PCH is built once (~10s for Boost.Spirit), then each split file compiles in ~1.2s instead of ~10s.
- **1-function rebuild**: From 23.6s to 11.4s. Without PCH, this was *slower* than monolithic (23.6s vs 18.2s). With PCH, it's now **42% faster** than monolithic.
- **Per-file speedup**: 8.5x — a single split file compiles in 1.2s with PCH vs 10.4s without.

The PCH transforms splitting from "mostly useful for no-change rebuilds" into a genuine incremental win for every edit.

### Where Splitting Wins

The benefit grows under specific conditions:

1. **Heavy headers**: Template-heavy libraries (Boost.Spirit, Eigen, Qt) benefit most from PCH. The heavier the headers, the larger the per-file savings.
2. **Many functions with substantial bodies**: When function code generation dominates over header parsing, skipping 16 out of 17 functions saves real time. Files with 50+ functions see proportionally larger savings.
3. **Moderate headers**: Files that include `<algorithm>`, `<string>`, `<vector>` (not extreme template libraries) have lower per-file parsing costs, making the parallel split build competitive with monolithic even without PCH.
4. **Frequent edits**: Over an afternoon of development, dozens of incremental rebuilds add up. Even modest per-rebuild savings compound into significant time saved.
5. **Larger files**: A 2,000-line file with 50 functions has more code generation work to skip per rebuild than our 510-line example.

## CMake Integration

cpp-splitter can act as a `CMAKE_CXX_COMPILER_LAUNCHER`, transparently intercepting compilation commands:

```bash
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/cpp-splitter ..
make -j$(nproc)
```

In this mode, it:
- Intercepts each `g++ -c -o foo.o foo.cpp` command
- Splits `foo.cpp` into per-function files in a `foo.o.split/` directory
- Compiles each split file separately (with parallelism)
- Combines the resulting `.o` files via `ld -r` (relocatable linking) into the single `foo.o` that Make/Ninja expects
- Passes non-compilation commands (linking, dependency generation) through unchanged

This is completely transparent to the build system. No CMakeLists.txt changes needed.

### Dependency Tracking

When the compiler generates dependency files (`-MD`, `-MMD`), cpp-splitter ensures the first split file runs sequentially to produce the `.d` file, then the remaining files compile in parallel. The dependency output correctly references the original source file, so Make's incremental rebuild logic works as expected.

## Limitations and Future Work

- **Cold build overhead**: Splitting adds overhead to the first build (PCH build + splitting + parallel compile). The tool is optimized for the edit-compile-test cycle, not CI/CD pipelines doing clean builds. With PCH, the cold build penalty is cut roughly in half for header-heavy files.
- **Template-heavy code**: Function templates must stay in the header (they can't be split into separate translation units). Files that are mostly templates see less benefit.
- **Link-time overhead**: More `.o` files mean slightly more work for the linker, though `ld -r` in launcher mode mitigates this by combining split objects before the final link.
- **Header changes invalidate everything**: If you modify a header included by the source file, all split files and the PCH need recompilation (same as monolithic). The savings come specifically from source file edits.

## Conclusion

cpp-splitter trades a one-time splitting cost for per-function incremental compilation granularity. With automatic precompiled headers, even template-heavy files like Boost.Spirit see genuine incremental wins — 1-function rebuilds are 42% faster than monolithic, and the cold build penalty is cut in half. The CMake launcher integration makes it a drop-in addition to existing build workflows, and the source-text-based approach ensures reliable forward declarations regardless of how complex your types are.

The tool is open source and available at the project repository. Try it on your heaviest `.cpp` file and see how much time you save on incremental rebuilds.
