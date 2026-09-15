# p4c: plain, unity build, split

The plain and unity columns from one run of `./benchmark-p4c.sh` on 15 September 2026
(TODO/47); the split column measured again the same day after TODO/48 (`MODES=split`), its
earlier figures kept below. Three configurations of the same tree, in the five scenarios of
the other benchmarks; the build phase is timed, configuration is not.

## What was built

p4c `8192431` (github.com/p4lang/p4c, 13 September 2026) without the control plane
(`ENABLE_CONTROL_PLANE=OFF`, which removes Protobuf and the backends that need it) and
without the GTest suite: `lib`, `ir`, `frontends`, `midend`, the `p4fmt` and `graphs`
backends and the ir-generator -- **218 C++ units of p4c's own** -- plus the 102 units of
Abseil, which p4c fetches and builds itself. Abseil is built plain in every configuration:
p4c keeps it out of the unity build (`cmake/Abseil.cmake`), and the launcher is withheld
from it the same way.

| | |
|---|---|
| Machine | AMD EPYC-Milan, 32 cores |
| Compiler | clang 13.0.0 (tipi toolchain `4f846ee`), `-std=gnu++20` as p4c sets it, libc++, **Release** (`-O3`) |
| Build system | CMake 3.31.9, ninja 1.12.1, `-j16` |
| Unity build | `-DCMAKE_UNITY_BUILD=ON`: 29 batches for the 218 p4c units, plus the two generated parser units on their own |
| Split | `cpp-splitter` as `CMAKE_CXX_COMPILER_LAUNCHER`, on the 218 p4c units |

p4c requires C++20, and clang 13 cannot compile libstdc++ 13's `<chrono>` in C++20 mode, so
the build uses the toolchain's libc++; the dependencies (Boost 1.85 built with libc++,
bison, flex, libgc, libgmp) are set up under `build/p4c-deps` without root. The script does
that.

## The five scenarios

**full** -- configure, then build. **no-op** -- build again. **one source** -- `touch
frontends/p4/callGraph.cpp`. **one header** -- `touch lib/cstring.h`, which every unit
includes. **one body** -- one line added to the body of an inline member of `cstring` in
`lib/cstring.h`, a header every unit includes. For plain and unity the member does not
matter: every includer recompiles whatever was edited, and the row is `cstring::size()`.
For the split it matters, and two are measured: `cstring::findlast()`, emitted by 2 units,
named by 11 more, and declared only in the other 205 copies (TODO/48); and
`cstring::size()`, emitted by 14 units and named by every unit (`size` is a member of every
container), so no copy declares it only.

## Results

Each ratio is *how many times faster*: `plain / split` above 1 means the split build was
faster than the plain one by that factor, below 1 slower.

| scenario | plain | unity | split | plain / unity | plain / split | unity / split | fallbacks | declined |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| full | 127.8s | 72.5s | 965.5s | 1.76x | 0.13x | 0.08x | 0 | 2 |
| no-op | 0.2s | 0.2s | 0.2s | — | — | — | 0 | 0 |
| one source | 3.7s | 10.0s | 0.6s | 0.37x | 6.2x | 16.7x | 0 | 0 |
| one header | 106.8s | 63.6s | **19.4s** | 1.68x | **5.5x** | **3.3x** | 0 | 2 |
| one body, `findlast()` | 102.6s | 63.9s | **23.0s** | 1.61x | **4.5x** | **2.8x** | 0 | 2 |
| one body, `size()` | 102.6s | 63.9s | 684.1s | 1.61x | 0.15x | 0.09x | 0 | 2 |

On the `findlast()` edit 203 units declare it only and change nothing, 2 re-slice their
piece, 2 PCHs are rebuilt and 50 pieces compile. On the `size()` edit 194 PCHs are rebuilt
and 9689 pieces compile. The split column before TODO/48 read 1064.1s, 0.2s, 0.7s, 19.1s
and 683.1s (`size()`); `findlast()` was not measured then and would have read like `size()`,
every copy defining it.

All three `p4fmt` binaries format the same program identically (`9121fd3014c8`), all three
`p4c-graphs` print the same version. Build trees: plain 326M, unity 250M, split 34G, with
10381 pieces.

**0 fallbacks, 2 declined**: `lib/cstring.cpp`, whose string interner is an
unnamed-namespace function holding a local static (one cache per piece otherwise), and the
bison-generated `ir-generator.cpp`, whose `yylval` has a type no other translation unit can
name and is used from a template. Both are compiled whole, correctly; the reasons are the
splitter's own.

## Reading the rows

**Unity is 1.76x faster than plain on the full build** (72.5s against 127.8s): 218 parses
of the IR headers become 29. On the incremental rows it is slower than plain when the edit
is in a source (one source: 10.0s against 3.7s, the whole batch recompiles) and 1.6–1.7x
faster when the edit is in a header every unit includes (one header: 63.6s against 106.8s,
again 29 parses against 218).

**The split's full build is 7.55x slower than plain** (965s) and 13x slower than unity:
10381 pieces, each loading a 60–85 MB preamble PCH that embeds p4c's IR headers. A piece of
`frontends/p4/def_use.cpp` compiles in 0.59s with the PCH's templates pre-instantiated
(`-fpch-instantiate-templates`), 1.97s without, 3.7s with no PCH at all; the plain unit
takes 7.1s. 133 pieces of one unit are still 78s of CPU against 7s.

**The header touch is 5.5x faster than plain and 3.3x faster than unity** (19.4s): every
unit reuses its split, its PCH is checked against the contents of its prerequisites and
kept, and nothing recompiles.

**The `size()` body edit is 6.7x slower than plain** (684s). The edited `size()` is emitted as a piece by 14
units; those re-slice and recompile one piece. The other 204 units keep `size()` defined in
class in their copy of `cstring.h`, so their copy changes, the preamble PCH built from it
is stale -- 194 PCHs rebuilt -- and every piece compiled against it is recompiled: 9689
piece compiles. This is the correct behaviour: a piece that inlined the old body has to be
rebuilt. The earlier body rows of `opencv-local-split.md` and
`boost-spirit-rbe-summary-9-Sep-2026.md` did not pay this cost because the pieces never
read the PCH (see below) and the up-to-date check looked only at the piece's own text and
the preamble: objects that had inlined the old body were kept. Those rows measured the
re-slice and left stale objects behind.

**The `findlast()` body edit is 4.5x faster than plain and 2.8x faster than unity** (23.0s
against 102.6s and 63.9s). After TODO/48 a
unit's copy of a header *declares* a definition the unit does not emit and names nowhere
else -- not in a template, whose calls libclang cannot resolve, not in a kept body, not in
an initialiser -- so an edit to that body leaves 203 copies, their PCHs and their pieces
untouched: 2 units re-slice one piece, 2 PCHs are rebuilt, 50 pieces compile. `size()` does
not qualify: every unit names `size` for one container or another, and a name is all a
template's call leaves to go by. Of the 755k kept definitions across the copies, 19852 are
declared only; the most frequent are `IR::ParameterList::getParameter` (504 copies),
`IR::IAnnotated::addOrReplaceAnnotation` (350) and `cstring::own` (207).

## What the run changed in the splitter

Splitting p4c found twelve defects, each fixed with a fixture (`TODO/47`); the two that
matter to every benchmark:

- **The preamble PCH was never used.** clang consults `<header>.gch/` only for a header
  named by `-include`, never for an `#include` directive in the source; the pieces reached
  the preamble through their own `#include` alone. Every measurement in `benchmarks/` before
  this one compiled its pieces without the PCH. The pieces now carry `-include <preamble>`,
  the PCH is built with `-fpch-instantiate-templates` and validated against the contents of
  its prerequisites.
- **A header edit did not rebuild the pieces that inlined the edited body.** Fixed by the
  same change: the PCH is rebuilt and the pieces recompile against it.

## Reproducing

```sh
./benchmark-p4c.sh            # clones p4c into example/p4c and sets up build/p4c-deps if absent
JOBS=8 ./benchmark-p4c.sh
```
