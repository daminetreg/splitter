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

Using an 18-function data processing module (374 lines, standard library headers):

| Scenario | Time |
|---|---|
| Monolithic full build | 3,013ms |
| Split + parallel compile (cold) | 11,360ms |
| Incremental rebuild (no changes) | 2,537ms |
| Incremental rebuild (1 function) | 4,067ms |

**Analysis**: For this small file, the cold split build is ~3.8x slower than monolithic — the overhead of spawning 18 compiler processes, each of which independently parses all the included headers, dominates. The incremental "no changes" case is slightly faster (16% savings) because it skips compilation entirely once it confirms nothing changed.

### Where Splitting Wins

The sweet spot for cpp-splitter is files that are:

1. **Large** — hundreds or thousands of lines with many functions
2. **Header-heavy** — including expensive headers like Boost, Qt, or deeply nested template libraries
3. **Frequently modified** — files you touch repeatedly during development

For a 500+ line file that includes Boost.Spirit (which can take 10-30 seconds for a monolithic compile), changing one function and recompiling just that split file saves the entire cost of re-parsing Boost headers for the unchanged functions. The incremental rebuild compiles only the changed function — a few hundred milliseconds instead of tens of seconds.

The overhead of splitting is a fixed cost paid once during the cold build. Every subsequent incremental build recoups that investment.

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

- **Cold build overhead**: Splitting always adds overhead to the first build. The tool is optimized for the edit-compile-test cycle, not CI/CD pipelines doing clean builds.
- **Template-heavy code**: Function templates must stay in the header (they can't be split into separate translation units). Files that are mostly templates see less benefit.
- **Link-time overhead**: More `.o` files mean slightly more work for the linker, though `ld -r` in launcher mode mitigates this by combining split objects before the final link.
- **Header changes invalidate everything**: If you modify a header included by the source file, all split files need recompilation (same as monolithic). The savings come specifically from source file edits.

## Conclusion

cpp-splitter trades a one-time splitting cost for per-function incremental compilation granularity. For large, header-heavy C++ files that you edit frequently, it can dramatically reduce the edit-compile-test cycle time. The CMake launcher integration makes it a drop-in addition to existing build workflows, and the source-text-based approach ensures reliable forward declarations regardless of how complex your types are.

The tool is open source and available at the project repository. Try it on your heaviest `.cpp` file and see how much time you save on incremental rebuilds.
