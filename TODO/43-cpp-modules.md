# 43 — C++20 named modules: where the splitter applies, and what it costs

Design only. Decisions taken: C++20 named modules first; the toolchain move to clang ≥ 16 is
part of this entry.

## Motivation

The splitter's measured result is about headers: a body edit in a header 194 units include
costs one piece instead of 194 recompiles. Named modules move that cost rather than remove
it. An importer does not re-parse the module's headers, so the "inline function in a central
header" case shrinks — but the BMI (`.pcm`) clang writes for an interface unit serialises
the whole unit, bodies included. Any body edit in an interface unit changes the BMI, and every
importer rebuilds, transitively through the module graph. A reduced BMI that carries
declarations only exists from clang 19 as `-fexperimental-modules-reduced-bmi`; it does not
cover implementation partitions or the importers' own units. The cascade is the one the
splitter exists to remove, with the edit in a `.cppm` instead of a header.

Measured on this machine, 13 September 2026, with the tipi toolchain (clang 13):

- clang 13 builds and consumes a named module by hand: `-x c++-module --precompile` writes
  the `.pcm`, `-c math.pcm` its object, `-fmodule-file=math.pcm` compiles an importer; the
  probe printed `42 7`. It has no `-fmodule-output`, so CMake's module support — CMake ≥ 3.28,
  ninja dyndep, `clang-scan-deps`, clang ≥ 16 — cannot drive it.
- The command-line tool does not take `.cppm` as a source: `is_source_file()` lists `.cpp`,
  `.cc`, `.cxx`, `.C`, `.c++`, `.cp`, `.c`, and the file name was passed to the shell.
- The launcher on an importer with `-fmodule-file=` took the *interface unit* for a header:
  `inclusions_of()` completes the candidate list from the AST with every file a definition
  sits in, the interface unit's definitions are in the AST through the import, and the
  mirrored copy fails with `module declaration must occur at the start of the translation
  unit`. The unit still built; the header split was reported as failed and skipped.
- Ubuntu 24.04's apt has `clang-18`, `libclang-18-dev` and `clang-tools-18` (18.1.3). The
  splitter already links a libclang other than the compiler's through
  `CPP_SPLITTER_LIBCLANG_ROOT` (`CMakeLists.txt`, used for macOS with Homebrew's llvm).

## Where the splitter applies under modules

In order of expected value.

1. **Interface units** (`export module M;`). Split into the interface — module declaration,
   global module fragment, imports, every declaration, exported `inline` bodies and templates
   kept, non-inline bodies replaced by declarations — which is what the compiler precompiles;
   and one **implementation unit per non-inline body**, `module M;` followed by the body,
   compiled against the BMI. The BMI then changes only when a declaration or an inline body
   changes. A careful author does this by hand; the splitter does it mechanically, as it does
   for headers.
2. **Implementation units and partitions** (`module M;`, `module M:part;`). The per-function
   split as for a `.cpp`; the preamble is the module declaration and imports rather than an
   include block.
3. **Importers** (`import M;` in a `.cpp`). The existing split. The preamble carries the
   `import` lines; the pieces are compiled with the same `-fmodule-file=` flags, which pass
   through already.
4. **Global module fragment** (`module;` then `#include`s, then `export module M;`). The
   prefix-PCH path applies to the includes before the module declaration.
5. **Header units** (`import <vector>;`). As 3.

What does not change: templates are instantiated in the importer, as with headers, and an
edit to an exported inline body rebuilds importers in either build.

## Costs and risks

- **Ordering inside one launcher call.** The BMI has to exist before its own pieces compile:
  precompile the interface, compile the pieces with `-fmodule-file=M=<pcm>`, then `ld -r` the
  interface object and the pieces into the object the build asked for. Header pieces follow a
  comparable order today.
- **The build system scans first.** CMake ≥ 3.28 runs `clang-scan-deps` before the compile
  and expects the launcher to write exactly the `.pcm` and `.o` it planned
  (`-fmodule-output=`). The launcher must keep the BMI path and module name the scanner saw.
  This needs clang ≥ 16, hence the toolchain move.
- **Distributed execution.** The BMI is an input of every importer's action and of every
  piece's; reproxy's input processor has to see it. Not known; an acceptance criterion
  measures it.
- **What may move.** Non-inline, non-template bodies only. An exported `inline` body belongs
  in the BMI. `export` cannot appear in an implementation unit, so a piece carries the body
  without the keyword and the declaration in the interface keeps it.
- **Incremental paths.** `split.cache`, `inputs.hash` and the harvest work unchanged; the BMI
  becomes a prerequisite the depfile names (clang lists it under `-MD` when modules are on).
- **libclang.** Parsing module units needs a libclang that understands them; libclang 18
  beside clang 18, through `CPP_SPLITTER_LIBCLANG_ROOT`.

## Implementation Proposal

### Phase 0 — correct on today's toolchain

- `is_source_file()`: add `.cppm`, `.ixx`, `.cxxm`, `.c++m`.
- `inclusions_of()`: a module unit is never a header candidate. Exclude a file whose first
  non-comment line is `module`, `export module` or `import`, and a file that no inclusion
  directive reached — one present in the AST only through an import.
- Fixture `launcher.importer_keeps_the_interface_whole` (`test/modules/`): the BMI built by
  the compiler by hand — skipped with the `cpp-splitter-test-skip:` marker when
  `--precompile` is unsupported — then the launcher on `use.cpp` with `-fmodule-file=`.
  Assert: no attempt to split the interface unit, no fallback, program prints `42 7 14`.
  Runs on clang 13.

### Phase 1 — toolchain move

- `environments/ubuntu-clang-18.cmake`, `.pkr.js` and container lock: the image installs
  `clang-18 libclang-18-dev clang-tools-18`; compiler `/usr/bin/clang++-18`;
  `CPP_SPLITTER_LIBCLANG_ROOT=/usr/lib/llvm-18`.
- Build the splitter against libclang 18 and run the whole suite. Known sensitivities:
  `probe_driver_standard()` (clang 18 defaults to gnu++17); `CXCursor_LinkageSpec` against
  `CXCursor_UnexposedDecl` (both accepted since TODO/42); libstdc++ 13 headers under clang 18;
  `-fuse-ld=lld` and the static libc++ link in `CMakeLists.txt`.
- CI: a job beside the clang 13 ones; clang 13 stays until every fixture passes on 18.

### Phase 2 — split an interface unit

- Detect a module unit from the source — `export module` or `module` after the optional
  global module fragment — and from `-x c++-module` on the command line.
- The preamble of an interface unit keeps everything and replaces non-inline bodies,
  exported or not, with declarations, keeping `export`. It is the interface unit the
  compiler sees.
- A piece is `module M;` on its first line and the body after it; no include of a preamble,
  since an implementation unit imports its interface implicitly. Compiled with
  `-fmodule-file=M=<pcm>`.
- Launcher flow for the interface unit's own command (`-x c++-module -c m.cppm
  -fmodule-output=<pcm> -o m.o` on clang ≥ 16): write the preamble; run the original command
  on it, which writes the BMI and the interface object; compile the pieces; `ld -r` into
  `m.o`; rewrite the depfile to name the source and the BMI.
- Harvest and re-slice unchanged; the BMI joins `inputs.hash`.

### Phase 3 — measure

- `example/modules/`: one module with about 40 exported non-inline functions and 30
  importers. Rows: full; one non-inline body in the interface; one inline body in the
  interface; one importer edited. Expected: the non-inline body edit recompiles one piece and
  no importer in the split build and every importer in the plain one; the inline body edit
  recompiles importers in both.
- On the cluster: the BMI in reclient's input records for a piece and for an importer.

## Representative example

`test/modules/math.cppm`:

```cpp
export module math;
export inline int twice(int v) { return v * 2; }   // inline: stays in the BMI
int helper_kept() { return 7; }                    // not exported: moved to a piece
export int seven() { return helper_kept(); }       // exported, non-inline: moved to a piece
export int plus_one(int v);                        // defined in the implementation unit
```

`test/modules/math_impl.cpp`:

```cpp
module math;
int plus_one(int v) { return v + 1; }
```

`test/modules/use.cpp`:

```cpp
import math;
#include <cstdio>
int describe() { return twice(3) + seven(); }
int main() { std::printf("%d %d %d\n", twice(21), seven(), plus_one(describe())); return 0; }
```

## The test

`launcher.module_interface_split`, `test/cmake/RunModuleSplitTest.cmake`, skipped with the
`cpp-splitter-test-skip:` marker when the compiler has no `-fmodule-output`:

1. The launcher on `math.cppm` (`-x c++-module -c -fmodule-output=math.pcm`), on
   `math_impl.cpp` and on `use.cpp` (both `-fmodule-file=math=math.pcm`); link; the program
   prints `42 7 14`; 0 fallbacks.
2. Pieces exist for `helper_kept` and `seven`, none for `twice`; the preamble holds
   `export int seven();` and `twice`'s body.
3. BMI stability: change `seven()`'s body, rerun the three calls; `math.pcm` is
   byte-identical, `use.o` is reported up to date, exactly one piece recompiled.
4. BMI change: change `twice()`'s body; `math.pcm` differs and `use.o` recompiles.
5. Byte-identity: the re-slice in step 3 produces, file for file, what a forced full split of
   the same edit produces, as `launcher.incremental_body_edit` checks.

Phase 0's `launcher.importer_keeps_the_interface_whole` runs on clang 13.

## Acceptance Criteria

- The Phase 0 fixture passes on clang 13 and the Phase 2 fixture on clang 18; each fails
  before its phase.
- The existing suite passes on clang 18 with libclang 18, and Boost.Filesystem and
  Boost.Spirit build with 0 fallbacks on it.
- `example/modules`: a non-inline body edit in the interface unit recompiles one piece and no
  importer in the split build, and every importer in the plain build.
- The BMI appears in reclient's input records for a piece and for an importer on the
  cluster, or an entry is filed with the measurement.
