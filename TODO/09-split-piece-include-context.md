# 09 — A header's split pieces are compiled without the includer's context

**Severity:** High. This is now the sole blocker for every translation unit that still
falls back on the Boost example.

**Found while implementing Step 2 of TODO 06**, which fixed the same problem on the
discovery side and made this the remaining half.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

A split piece extracted from a header includes exactly one thing — that header's rewritten
copy:

```cpp
#include "std_category_impl.hpp"
```

Many headers are not self-contained. They are written to be included at a particular point
in a particular order, after other headers have completed the types they use. Compiling one
alone fails even though the header is perfectly valid where it is actually included.

`boost/system/detail/std_category_impl.hpp` is the clean example. It is included only after
`boost::system::error_condition` is complete, so its split piece fails with:

```
std_category.hpp:71:21: error: calling 'default_error_condition' with incomplete return
                        type 'boost::system::error_condition'
```

TODO 06 Step 2 removed this requirement from *discovery* — headers are no longer parsed
standalone, their inventories come from the translation unit that includes them, in the
right context. But the generated split piece then throws that context away and includes the
header on its own, so the requirement returns at compile time.

On the Boost `filesystem` build this accounts for the bulk of what remains: 396
`no member named`, 80 `constexpr variable`, 68 `non-constexpr declaration of`, and the
`futex.hpp` and `path.hpp` header-dependency failures. Four of twelve translation units
still fall back because of it.

## Description

`emit_split_files()` writes each piece as a comment header, one `#include` of the unit's
preamble, and the function body. For a translation unit that is correct: the preamble is
that `.cpp` with the bodies carved out, so it carries the full include prefix the source
had. For a header it is not: the preamble is only that header, and the prefix its includer
established is absent.

### Implementation plan

1. Record, for each split header, the *includer* context it was harvested in — at minimum
   the translation unit whose parse produced the inventory. `resolve_header_deps()` already
   knows this; it needs to reach `split_unit()` and be stored in the header's manifest.
2. Emit the includer's prefix ahead of the header in each split piece. The natural form is
   to include the translation unit's own preamble first, then the header:

   ```cpp
   #include "<tu>_preamble.h"     // re-establishes the context the header was seen in
   #include "std_category_impl.hpp"
   ```

   The TU preamble already includes everything the source included, in order, so this
   reproduces the header's real context. Check that including it does not pull in the TU's
   own split-out declarations in a way that conflicts.
3. An alternative worth measuring: instead of including the rewritten header, have the
   piece include the *original* header through the includer's preamble and rely on the
   forward declarations already generated. This avoids rewriting the header at all for the
   compile step, and would interact well with removing the rewritten copy from the include
   path (TODO 04's mirrored tree would then only matter for the pieces).
4. Headers included by more than one translation unit already get one split directory per
   translation unit, so per-TU context is representable without sharing conflicts. Confirm
   the manifest and `header_obj_files` de-duplication still hold once context is recorded.
5. Re-check TODO 07's weak-symbol merging: pieces from the same header compiled under two
   different TU contexts must still produce mergeable definitions.

## Acceptance Criteria

- `boost/system/detail/std_category_impl.hpp`, `boost/atomic/detail/futex.hpp` and
  `boost/filesystem/path.hpp` split pieces compile.
- The `no member named`, `constexpr variable` and `non-constexpr declaration of` error
  classes drop to zero on the Boost `filesystem` build.
- More than two of the twelve translation units link from split objects.
- Regression fixture: a header that requires a type completed by an earlier include, split
  and compiled through its includer — must compile.
- No duplicate-symbol errors when two translation units both split the same header.

## Outcome

Implemented in `src/main.cpp`. The header case needed three fixes, not one -- the include
directive was only the visible third of it.

1. **Context preamble.** A split piece taken from a header now includes the translation
   unit's preamble before the header itself:

   ```cpp
   #include "portability_preamble.h"   // replays the includer's prefix
   #include "path.hpp"
   ```

   `split_unit()` and `emit_split_files()` take a `context_preamble`, empty for the
   translation unit itself and set by `resolve_header_deps()` for each header.

2. **Include order.** The split tree was appended *after* the project's own `-I` flags, so
   an original header won the lookup over its rewritten copy -- bringing back the very
   definitions that had been moved into split pieces. Adding the context preamble made this
   visible immediately: 1008 `redefinition of` errors and every translation unit falling
   back. The split directory and its mirrored include root now precede the project flags.
   (The TODO 04 write-up asserted this ordering was already the case. It was not.)

3. **The source's own directory is an implicit include directory.** A quote include
   resolves relative to the including file first. The preamble carries the source's quote
   includes but no longer sits in the source's directory, so `#include "ctx_base.hpp"`
   could not resolve; and a header sitting beside its source fell to the `_abs` hash
   fallback in `header_mirror_relpath()`, leaving its rewritten copy unreachable at the path
   the source spells. `unit_include_dirs()` now appends the unit's source directory, and the
   compile commands add `-I<source dir>` after the split tree.

A fourth, smaller fix came out of the same run: a free function split out of a header kept
its default arguments while the generated declaration also carried them, which is
`redefinition of default argument`. `prepare_functions()` now strips default arguments from
the definition of non-members as it already did for members. That is what cleared
`boost/atomic/detail/futex.hpp` and let `lock_pool.cpp` link.

Verified on the Boost `filesystem` build:

| check | before | after |
|---|---|---|
| total `error:` lines | 1110 | **183** |
| `no member named` | 396 | **20** |
| `constexpr variable` | 80 | **48** |
| `non-constexpr declaration of` | 68 | **0** |
| header-dependency compile failures | 3 | **0** |
| translation units linking from split objects | 2 | **3** |
| translation units falling back | 9 | **8** |
| wall time | 26.5s | **13.2s** |

`test/ctx_main.cpp` with `ctx_base.hpp` and `ctx_user.hpp` is the fixture the plan asked
for: `ctx_user.hpp` deliberately does not include the header that completes the type it
uses, so it is only valid in its includer's context. It splits, compiles, links and runs
with no fallback. The other three fixtures also run with no fallback, the two translation
unit header test still links without duplicate symbols, and an incremental rebuild
reproduces the same 5134 split files with no differences.

### Not fully met

Two acceptance criteria are only partly met: `no member named` reached 20 rather than 0,
and `constexpr variable` 48 rather than 0. What remains is no longer include context -- the
residue is 48 `constexpr variable ... must be initialized by a constant expression`, 20
`no member named`, and 4 `only virtual member functions can be marked 'override'`, which is
a gap in the specifier stripping from TODO 05. Eight of twelve translation units still fall
back, now on their own split pieces rather than on header dependencies.
