# 43 — C++20 named modules: where the splitter applies, and what it costs

Design only. Decisions taken: C++20 named modules first; the toolchain move to clang ≥ 20 is
part of this entry. Rewritten on 14 September 2026 after measuring what a BMI holds, with
and without `-fmodules-reduced-bmi` — the first version assumed the whole unit was
serialised, which is true of the full BMI only, and the question was whether the reduced
BMI already gives what the splitter would.

## Motivation

The splitter's measured result is about headers: a body edit in a header 194 units include
costs one piece instead of 194 recompiles. Named modules move that cost rather than remove
it. An importer does not re-parse the module's headers; it reads the BMI (`.pcm`) the
interface unit's compile writes, and it rebuilds whenever that BMI changes. What is in the
BMI, and what changes it, therefore decides where the splitter has anything to do.

Measured 14 September 2026 with Homebrew clang 21.1.7 on macOS (the tipi clang 13 cannot
drive CMake modules, see below). `example/cpp-20-modules/bmi-probe/probe.sh` reproduces
every line; the module has an exported `inline` function, a template, an exported and a
non-exported non-inline function, an in-class and an out-of-line member definition, and a
function defined in an implementation unit.

- **The full BMI holds every body.** 130 KB for the probe, 17.4 MB for Kitware's one-class
  `foo` with `<iostream>` in its global module fragment.
- **The reduced BMI (`-fmodules-reduced-bmi`, clang ≥ 20; `-fexperimental-…` in 19) drops
  the bodies of non-inline functions**, exported or not: 61 KB, three `STMT_COMPOUND`
  records instead of eight. It keeps `inline` functions and templates, which importers
  instantiate or emit themselves. An in-class member definition counts as non-inline — in a
  named module it is not implicitly `inline` (P1779) — so its body is dropped too and the
  module's own object carries it as a strong symbol.
- **The reduced BMI still changes on every body edit**, including a same-length edit of a
  non-exported, non-inline body whose statements it does not contain. Exactly one record
  differs, `DECL_FUNCTION`, in one operand: the definition's **ODR hash**, which clang
  computes from the body and stores on the declaration for cross-module ODR checks. It is
  not the timestamp (`-fno-pch-timestamp`, a later rebuild with no edit: identical) and no
  flag drops it for module-attached declarations (`-fskip-odr-check-in-gmf` covers the
  global module fragment only).
- **An interface unit that declares the function and leaves the body to an implementation
  unit produces a byte-identical BMI across edits of that body.** That is the split shape.
- **CMake + Ninja rebuild every importer on any edit to the interface unit regardless.**
  The module compile edge has no `restat`; `ninja -d explain` on Kitware's example shows
  `main.cxx.o is dirty` after a body edit in `foo.cxx`. BMI stability is worth nothing to
  the mtime-driven build; it is worth everything to content-addressed caching — cmake-re's
  cache and reclient's action digests include the `.pcm` — and to the launcher, whose
  `inputs.hash` already keys on content and can report an importer's pieces up to date and
  only relink them.
- GCC: not measured (no GCC on this machine, Docker down). Its CMI (`.gcm`) streams only
  declarations plus inline, template and constexpr definitions by design, so it is
  "reduced" already; whether it also records a hash of the dropped bodies is the open
  question and an acceptance criterion below.

So the reduced BMI answers the size question and not the cascade one. The cascade — a body
edit in the interface unit rebuilds, or re-fetches, every importer transitively — is the one
the splitter exists to remove, with the edit in a `.cppm` instead of a header, and the only
thing that removes it is the body not being in the interface unit at all. A careful author
does this by hand; the splitter does it mechanically, as it does for headers.

Earlier findings (13 September 2026, tipi clang 13) that still stand:

- clang 13 builds and consumes a named module by hand (`-x c++-module --precompile`,
  `-fmodule-file=`) but has no `-fmodule-output`, so CMake's module support — CMake ≥ 3.28,
  ninja dyndep, `clang-scan-deps`, clang ≥ 16 — cannot drive it. Apple's clang cannot
  either: the command-line tools ship no `clang-scan-deps`.
- The command-line tool does not take `.cppm` as a source (`is_source_file()`).
- The launcher on an importer with `-fmodule-file=` took the *interface unit* for a header:
  `inclusions_of()` completes the candidate list from the AST with every file a definition
  sits in, and the interface unit's definitions are in the AST through the import. The
  mirrored copy fails with `module declaration must occur at the start of the translation
  unit`; the unit still built, the header split was reported failed and skipped.

## Where the splitter applies under modules

In order of expected value.

1. **Interface units** (`export module M;`). Split into the interface — module declaration,
   global module fragment, imports, every declaration, `inline` bodies and templates kept,
   non-inline bodies replaced by declarations — which is what the compiler precompiles;
   and one **implementation unit per non-inline body**, `module M;` followed by the body,
   compiled against the BMI. The BMI, reduced or full, then changes only when a declaration
   or an inline body changes. What may move is wider than for headers: an in-class member
   body is non-inline here, so it may move too, at the cost of rewriting the class body
   (`int n() { … }` → `int n();` plus `int S::n() { … }` in the piece) — the harvest's
   member-emission work of TODO/05, a later step.
2. **Implementation units and partitions** (`module M;`, `module M:part;`). The
   per-function split as for a `.cpp`; the preamble is the module declaration and imports
   rather than an include block.
3. **Importers** (`import M;` in a `.cpp`). The existing split. The preamble carries the
   `import` lines; the pieces are compiled with the same `-fmodule-file=` flags, which pass
   through already. With the BMI byte-stable, `inputs.hash` reports the pieces up to date
   and the launcher only relinks — which is how the split build beats ninja's own dirtiness.
4. **Global module fragment** (`module;` then `#include`s, then `export module M;`). The
   prefix-PCH path applies to the includes before the module declaration.
5. **Header units** (`import <vector>;`). As 3.

What does not change: templates are instantiated in the importer, as with headers, and an
edit to an `inline` body rebuilds importers in either build. The reduced BMI is orthogonal
and should simply be on in the split build: smaller file, less to hash, less to load.

## Costs and risks

- **Ordering inside one launcher call.** The BMI has to exist before its own pieces compile:
  precompile the interface, compile the pieces with `-fmodule-file=M=<pcm>`, then `ld -r`
  the interface object and the pieces into the object the build asked for. Header pieces
  follow a comparable order today.
- **The build system scans first.** CMake ≥ 3.28 runs `clang-scan-deps` before the compile
  and expects the launcher to write exactly the `.pcm` and `.o` it planned (through the
  `@….modmap` response file: `-x c++-module -fmodule-output=<pcm>`). The launcher must keep
  the BMI path and module name the scanner saw. This needs clang ≥ 16, and the reduced BMI
  clang ≥ 20, hence the toolchain move.
- **The launcher must not rewrite an unchanged BMI.** Ninja rebuilds importers in the same
  run whatever the bytes; content caches and `inputs.hash` do not. Write the BMI to a
  temporary path and replace only on difference, so nothing downstream sees a new digest.
- **Distributed execution.** The BMI is an input of every importer's action and of every
  piece's; reproxy's input processor has to see it. Not known; an acceptance criterion
  measures it.
- **What may move.** Non-inline, non-template, non-`constexpr` bodies. An `inline` body
  belongs in the BMI. `export` cannot appear in an implementation unit, so a piece carries
  the body without the keyword and the declaration in the interface keeps it.
- **Incremental paths.** `split.cache`, `inputs.hash` and the harvest work unchanged; the BMI
  becomes a prerequisite the depfile names (clang lists it under `-MD` when modules are on).
- **libclang.** Parsing module units needs a libclang that understands them; libclang 20
  beside clang 20, through `CPP_SPLITTER_LIBCLANG_ROOT` (already used on macOS for
  Homebrew's llvm).

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

- `environments/ubuntu-clang-20.cmake`, `.pkr.js` and container lock: the image installs
  clang 20 with `libclang-20-dev` and `clang-tools-20` (apt.llvm.org on Ubuntu 24.04, whose
  own archive stops at 18 — 18 would do for CMake modules but not for the reduced BMI);
  compiler `/usr/bin/clang++-20`; `CPP_SPLITTER_LIBCLANG_ROOT=/usr/lib/llvm-20`. On macOS
  Homebrew's llvm 21 already serves, with `CMAKE_OSX_SYSROOT` set
  (`example/cpp-20-modules/README.md`).
- Build the splitter against libclang 20 and run the whole suite. Known sensitivities:
  `probe_driver_standard()` (newer clang defaults to gnu++17); `CXCursor_LinkageSpec`
  against `CXCursor_UnexposedDecl` (both accepted since TODO/42); libstdc++ 13 headers
  under a newer clang; `-fuse-ld=lld` and the static libc++ link in `CMakeLists.txt`.
- CI: a job beside the clang 13 ones; clang 13 stays until every fixture passes on 20.
- `example.cpp_20_modules` (TODO/47) then runs instead of skipping on the Linux job.

### Phase 2 — split an interface unit

- Detect a module unit from the source — `export module` or `module` after the optional
  global module fragment — and from `-x c++-module` on the command line.
- The preamble of an interface unit keeps everything and replaces non-inline bodies,
  exported or not, with declarations, keeping `export`. It is the interface unit the
  compiler sees. In-class member bodies stay in place in this phase (they move with
  TODO/05's member emission).
- A piece is `module M;` on its first line and the body after it; no include of a preamble,
  since an implementation unit imports its interface implicitly. Compiled with
  `-fmodule-file=M=<pcm>`.
- Launcher flow for the interface unit's own command (`-x c++-module -c m.cppm
  -fmodule-output=<pcm> -o m.o`, flags possibly inside an `@…modmap`): write the preamble;
  run the original command on it, which writes the BMI and the interface object — the BMI to
  a temporary path, moved over the planned one only if it differs; compile the pieces;
  `ld -r` into `m.o`; rewrite the depfile to name the source and the BMI.
- Harvest and re-slice unchanged; the BMI joins `inputs.hash`.

### Phase 3 — measure

- `example/modules/`: one module with about 40 exported non-inline functions and 30
  importers, reduced BMI on. Rows: full; one non-inline body in the interface; one inline
  body in the interface; one importer edited. Expected, with the launcher's up-to-date
  check counting: the non-inline body edit recompiles one piece and no importer in the
  split build and every importer in the plain one, reduced BMI or not; the inline body edit
  recompiles importers in both.
- Same rows through cmake-re's cache: cache hits for the importers in the split build.
- On the cluster: the BMI in reclient's input records for a piece and for an importer.
- GCC 14/15, in a container: the probe's steps 2 and 3 on the `.gcm`.

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

## Expected result on the example

What the splitter writes for the three files, and what the compiler is given.

`math.cppm` — the launcher's command is the interface unit's own
(`-x c++-module -c math.cppm -fmodule-output=math.pcm -fmodules-reduced-bmi -o math.o`).

```
math.o.split/
├── math_interface.cppm             the interface the compiler sees; precompiled to math.pcm
│                                   and compiled to math_interface.o by the original command
├── math.cppm_1_helper_kept.cpp     module math;  + the body         (implementation unit)
├── math.cppm_2_seven.cpp           module math;  + the body         (implementation unit)
├── math.cppm.harvest / .keeps
└── *.o                             ld -r → math.o, the object the build asked for
```

```cpp
// math_interface.cppm
export module math;
export inline int twice(int v) { return v * 2; }   // inline: stays, so it is in the BMI
int helper_kept();                                 // declaration left in place
export int seven();                                // declaration left in place, `export` kept
export int plus_one(int v);                        // as written
```

```cpp
// math.cppm_2_seven.cpp
module math;
#line 4 "/…/math.cppm"
int seven() { return helper_kept(); }
```

The piece is compiled with `-fmodule-file=math=math.o.split/math.pcm`, after the interface;
an implementation unit imports its interface implicitly, so no `#include` of a preamble.
`.keeps` records `twice` as *inline: belongs in the BMI* and `plus_one` as *declared only*.
An edit to `seven()`'s body changes `math.cppm_2_seven.cpp` and nothing else: the interface
text, and so `math.pcm`, is byte-identical — the probe's step 3, measured. Without the
split the same edit changes `math.pcm` through `seven`'s ODR hash, reduced BMI or not — the
probe's step 2.

`math_impl.cpp` — an implementation unit; every definition in it is a piece, each an
implementation unit of its own. There is no preamble to include: `module math;` gives each
piece the interface, and the unit's own file-local declarations, if any, go in a
`math_impl_preamble.h` the pieces include after the module declaration.

```cpp
// math_impl.cpp_1_plus_one.cpp
module math;
#line 2 "/…/math_impl.cpp"
int plus_one(int v) { return v + 1; }
```

`use.cpp` — an importer. An `import` may not appear in an included header, so the pieces
carry the unit's import declarations themselves, ahead of the preamble; the preamble holds
what it holds today, the include block and the declarations.

```
use.o.split/
├── use_preamble.h                  #include <cstdio>   int describe();
├── use.cpp_1_describe.cpp          import math;  #include "use_preamble.h"  + the body
├── use.cpp_definitions.h           main, which stays with the definitions
├── use.cpp_0_definitions.cpp       import math;  #include "use.cpp_definitions.h"
└── *.o                             ld -r → use.o
```

```cpp
// use.cpp_1_describe.cpp
import math;
#include "use_preamble.h"
#line 4 "/…/use.cpp"
int describe() { return twice(3) + seven(); }
```

Each piece is compiled with the unit's own `-fmodule-file=` flags, which pass through
already. The depfile names `use.cpp`, `<cstdio>`'s headers and `math.pcm`.

## How to look inside a BMI

- `clang++ -module-file-info m.pcm`: module name, structure, language options, inputs.
- `clang++ -cc1 -ast-dump-all m.pcm`: the declarations; bodies show only for declarations
  already deserialised (`used`), so absence there proves nothing.
- `llvm-bcanalyzer -dump m.pcm | grep -c STMT_COMPOUND`: bodies serialised, not lazy; and
  `diff` of two dumps names the record that changed (`DECL_FUNCTION` for the ODR hash).
- `-Xclang -fno-pch-timestamp` before comparing bytes; compile the same path both times,
  since `ORIGINAL_FILE` and `INPUT_FILE` records carry the name.

## The test

`launcher.module_interface_split`, `test/cmake/RunModuleSplitTest.cmake`, skipped with the
`cpp-splitter-test-skip:` marker when the compiler has no `-fmodule-output`:

1. The launcher on `math.cppm` (`-x c++-module -c -fmodule-output=math.pcm`, and
   `-fmodules-reduced-bmi` when the compiler takes it), on `math_impl.cpp` and on `use.cpp`
   (both `-fmodule-file=math=math.pcm`); link; the program prints `42 7 14`; 0 fallbacks.
2. Pieces exist for `helper_kept` and `seven`, none for `twice`; the preamble holds
   `export int seven();` and `twice`'s body.
3. BMI stability: change `seven()`'s body, rerun the three calls; `math.pcm` is
   byte-identical and keeps its mtime, `use.o` is reported up to date, exactly one piece
   recompiled. Control: the same edit compiled plain changes `math.pcm`.
4. BMI change: change `twice()`'s body; `math.pcm` differs and `use.o` recompiles.
5. Byte-identity: the re-slice in step 3 produces, file for file, what a forced full split of
   the same edit produces, as `launcher.incremental_body_edit` checks.

Phase 0's `launcher.importer_keeps_the_interface_whole` runs on clang 13.

## Acceptance Criteria

- The Phase 0 fixture passes on clang 13 and the Phase 2 fixture on clang 20; each fails
  before its phase.
- The existing suite passes on clang 20 with libclang 20, and Boost.Filesystem and
  Boost.Spirit build with 0 fallbacks on it.
- `example/modules`: a non-inline body edit in the interface unit recompiles one piece and no
  importer in the split build, and every importer in the plain build with
  `-fmodules-reduced-bmi` on — the control that the reduced BMI alone does not do it.
- `example/cpp-20-modules/bmi-probe/probe.sh` prints the four `DIFFERS` and the final
  `IDENTICAL` on the clang the Linux job uses; the same probe on GCC 14/15 records whether
  the `.gcm` is stable under a non-inline body edit, and the design's GCC paragraph is
  updated with the answer.
- The BMI appears in reclient's input records for a piece and for an importer on the
  cluster, or an entry is filed with the measurement.
