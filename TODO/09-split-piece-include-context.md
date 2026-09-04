# 09 — A header's split pieces are compiled without the includer's context

**Severity:** High. This is now the sole blocker for every translation unit that still
falls back on the Boost example.

**Found while implementing Step 2 of TODO 06**, which fixed the same problem on the
discovery side and made this the remaining half.

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
