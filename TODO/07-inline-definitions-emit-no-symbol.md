# 07 — Split-out `inline` definitions emit no symbol

**Severity:** High. Produces objects with dangling references rather than a build error,
so the failure surfaces late — at final link, in whatever consumes the library.

**Found while implementing TODO 02**, by a regression fixture that split cleanly and then
failed to link.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

A function that is `inline` in the original source keeps its `inline` keyword when its
body is written into a split `.cpp`, while the forward declaration generated into the
preamble has `inline` stripped. The two disagree: callers in other split units are
compiled against a declaration promising an ordinary external symbol, but the split unit
that owns the definition emits nothing, because an `inline` function that is not odr-used
within its own translation unit needs no out-of-line copy.

Minimal reproduction — an `inline` function with internal linkage, split and linked:

```
undefined reference to `__static_static_specifier__find_sep(char const*, unsigned long)'
```

`nm` on the split object confirms the definition is simply absent.

This is the design as documented. `DOCS.md` states the tool "extract[s] inline functions
from header files into separate .cpp files, keeping the `inline` keyword on bodies and
stripping it from forward declarations" and "uses `-fkeep-inline-functions` to force
symbol emission". That mechanism does not work with this project's toolchain:

```
clang++: warning: optimization flag '-fkeep-inline-functions' is not supported
                  [-Wignored-optimization-argument]
```

`-fkeep-inline-functions` is a GCC option. Clang parses and ignores it. Since the build
uses clang (`environments/monolithic.cmake` pins
`/usr/local/share/.tipi/clang/4f846ee/bin/clang++`), every split-out `inline` definition
currently relies on a no-op flag.

Header splitting is entirely built on extracting `inline` functions, so this affects the
whole header-splitting feature, not an edge case.

## Description

Three interacting pieces:

- `generate_forward_decl()` strips `inline` from the declaration written into the
  preamble (`strip_decl_specifier(sig, "inline")` as of TODO 02).
- The body emission in `emit_split_files()` strips only `static`, never `inline`, so the
  definition keeps it.
- The launcher adds `-fkeep-inline-functions` when compiling header-dependency objects,
  which clang ignores.

The result compiles cleanly and links only by luck — if some other translation unit
happens to emit the same inline function, the reference resolves; otherwise it does not.

### Implementation plan

Decide between two coherent designs, rather than the current mix of both:

**Option A — strip `inline` from the split-out definition (recommended).**
The split `.cpp` becomes the single out-of-line definition with external linkage,
matching the declaration already emitted into the preamble.

1. Apply `strip_decl_specifier(body, "inline")` alongside the existing `static` strip in
   `emit_split_files()`.
2. Ensure exactly one split unit owns the definition, so no duplicate-symbol errors when
   several translation units include the same split header — the existing per-header
   manifest and `header_obj_files` deduplication is the place to enforce this.
3. Drop `-fkeep-inline-functions` from the compile commands; it is dead weight under
   clang and misleading under GCC.
4. Note the semantic consequence: the function is no longer inlinable across translation
   units. This is inherent to splitting and should be stated in `DOCS.md`.

**Option B — keep `inline` and force emission portably.**
Add an explicit instantiation-like forcing construct in the split file (for example
taking the function's address into a `static` variable, or an
`__attribute__((used))`-annotated alias) instead of relying on a compiler flag. Preserves
inlining semantics for other translation units at the cost of a more fragile emitter.

Option A is simpler and matches what the preamble already declares. Whichever is chosen,
`DOCS.md`'s claim about `-fkeep-inline-functions` must be corrected.

## Acceptance Criteria

- An `inline` function with internal linkage, split out of a `.cpp`, links successfully:
  restore the `inline` keyword on `find_sep` in `test/static_specifier.cpp` (see the
  comment there) and the fixture must still split, compile, link and run with exit 0.
- `nm --defined-only` on the split object shows a definition for every function listed in
  that unit's `compilable_files`.
- No compile command passes `-fkeep-inline-functions` to clang, or the project documents
  why it is retained for GCC.
- A split header included by two translation units produces exactly one definition of
  each extracted inline function across the final link — no duplicate symbols.
- `DOCS.md`'s description of inline handling matches the implemented design.

## Outcome

Neither option alone works: **Option A is right for a `.cpp`, Option B for a header**, and
which one applies is decided by where the definition came from.

- A definition taken out of a `.cpp` is the only one that will ever exist, so `inline` is
  stripped (`strip_decl_specifier(body, "inline")`) and it becomes an ordinary strong
  symbol, matching the declaration the preamble already emits.
- A definition taken out of a *header* cannot do that. Each translation unit including the
  header gets its own split directory and its own object, so several objects carry the same
  function; Option A would turn that into duplicate strong symbols. `inline` therefore
  stays -- vague linkage is exactly what lets the linker merge the copies -- and the symbol
  is instead forced into existence with `__attribute__((used))`, emitted just before the
  definition so the `#line` mapping is unaffected.
- `-fkeep-inline-functions` is gone from both the launcher and CLI compile commands. It is
  a GCC option; clang parses and ignores it (`warning: optimization flag
  '-fkeep-inline-functions' is not supported`), so it never forced anything here.
- `DOCS.md` gained a "Linkage of split definitions" entry and no longer claims
  `-fkeep-inline-functions` does the work.

Verified:

| check | result |
|---|---|
| `inline` restored on `find_sep` in `test/static_specifier.cpp` | splits, compiles, links, runs (exit 0) |
| symbol for that `.cpp`-split function | `T` -- strong, `inline` stripped |
| symbol for a header-split function | `W` -- weak, vague linkage preserved |
| header split into two translation units, both objects linked | links with no duplicate symbol, runs (exit 0) |
| split objects carrying no definition at all | 0 of 14 |
| `-fkeep-inline-functions` in any compile command | none |

`test/inline_header.hpp` with `test/inline_a.cpp` and `test/inline_b.cpp` is the two
translation unit case: both objects define `demo::weigh` and `demo::span` as weak symbols,
and linking them together yields exactly one definition of each.

On the Boost `filesystem` build the translation-unit outcome is unchanged -- the four that
fail still fail, on missing declaration context in split headers (**TODO 06**) -- because
they fall back before reaching a link. The `-fkeep-inline-functions` warnings are gone.
