# 29 — Reparse, the serialized AST, and the preamble that is not waste

**Severity:** None on its own. This is the design note behind `TODO/28`, and the reason its
saving has to come from one particular direction.

## Which preamble

Three different things in this codebase are called "the preamble", and a measurement that
mixes them points at the wrong fix. Keeping them apart is the point of this file.

| | what it is | what it speeds up |
|---|---|---|
| **preamble PCH** (`build_pch`) | `<tag>_preamble.h.gch`, found by placement beside the header every piece includes | **compiling the split pieces** -- amortised over hundreds of them per unit |
| **prefix PCH** (`build_libclang_pch`) | the leading include block, fed back with `-include-pch` | the splitter's own parse |
| **`CXTranslationUnit_PrecompiledPreamble`** | libclang's implicit preamble for a TU it may reparse | the splitter's own parse |

**The preamble PCH is not a candidate for removal and never was.** It is the reason a unit that
splits into several hundred pieces can be compiled at all: without it each piece would compile
the whole carved-out translation unit from source. Everything below is about the other two.

The splitter's own parse is what should get faster, and `clang_reparseTranslationUnit` was
considered for it before -- the removed server actually used it.

## The question

`TODO/28` wants a body-only edit to skip the parse. libclang has an API built for exactly
that: `clang_reparseTranslationUnit()`, which an editor calls after every keystroke.

An earlier draft of this file dismissed it on the grounds that the launcher is one shot per
translation unit and that a serialized TU is no substitute. The first half is right, the second
was asserted without evidence and against this project's own history. The corrected version is
below, with the numbers that were missing.

## What this project already did

Commit `5d5f6c2` ("Improve build performance with AST caching") parsed with
`CXTranslationUnit_ForSerialization`, wrote `<stem>.ast` beside the output, and on a later run
loaded it with `clang_createTranslationUnit2()` instead of parsing:

```cpp
if (fs::exists(ast_cache) && fs::last_write_time(ast_cache) >= fs::last_write_time(abs_path)) {
    CXErrorCode cerr = clang_createTranslationUnit2(index, ast_cache.c_str(), &tu);
```

**That was outside server mode**, and it worked. It is not what the removed server did: the
server's `g_tu_cache` (`19416e0`) held live `CXTranslationUnit`s in memory across launcher
invocations, and it is there — and only there — that `clang_reparseTranslationUnit()` was
called.

The disk cache never reparsed. It was gated on the main file's mtime, so it was used only when
nothing had changed, which is what `split.cache` does today from a content hash over every
prerequisite rather than one mtime — strictly better, and the reason the `.ast` cache is gone.

## Measured: what each of these actually costs

`libs/geometry/test/algorithms/area/area.cpp`, its real 85 build flags, clang 13.0.0:

| | |
|---|---:|
| `clang_parseTranslationUnit2`, no flags | 1.43s |
| the same, `ForSerialization` | 1.44s |
| the same, `PrecompiledPreamble \| CreatePreambleOnFirstParse` | **2.28s** |
| `clang_saveTranslationUnit` | 0.61s |
| `clang_createTranslationUnit2` (open the `.ast`) | 0.02s — **but see below** |
| `clang_reparseTranslationUnit` on a **live** TU | 0 (success) |
| `clang_reparseTranslationUnit` on a **loaded** TU | **1 (failure)** |

Three conclusions, in ascending order of usefulness.

### 1. Reparse cannot be revived through the disk

A TU restored with `clang_createTranslationUnit2()` refuses to reparse — measured, not read off
the documentation. So the editor workflow (keep a TU, reparse it after each edit) needs a live
process, and the live process is the server that `TODO/01` removed because its absence silently
disabled splitting. Reintroducing that failure mode to buy an optimisation is the wrong trade.

### 2. Reparse would not help this row anyway

Reparse is cheap only because of the precompiled preamble: the leading `#include` block is
compiled once and reused, so only the text below it is re-parsed. Any change to a file *inside*
that preamble discards it.

`TODO/28`'s row is a **header** edit — `side_info.hpp`, reached through the include block, so
inside the preamble by construction. The preamble would be rebuilt and nothing saved. Reparse
helps when the main file changes below its own includes, which is the `one source` scenario the
split cache already wins 3.3x on.

### 3. Loading a serialized AST is 1.6x cheaper than parsing, not 70x

The 0.02s above is an **open**, not a usable AST. `clang_createTranslationUnit2()`
deserializes lazily: cursors are materialised on demand, so the cost does not disappear, it
moves into the first thing that walks the tree. The splitter walks it twice — once to harvest
and once in `collect_emitted()` — asking each cursor for its kind, spelling, extent and
linkage. Walking the same 825728 cursors both ways:

| | obtain | walk 1 | walk 2 | **total** |
|---|---:|---:|---:|---:|
| parse | 1.46s | 0.25s | 0.24s | **1.95s** |
| load `.ast` | 0.02s | **0.89s** | 0.31s | **1.21s** |

**1.6x, and 0.74s saved.** Against a re-split that costs ~10s over the cache-hit baseline, that
is 7%. It also costs 0.61s to write the `.ast` on every cold split and a large file to keep, so
a good part of the saving is spent up front.

An earlier version of this file quoted the 0.02s as "70x cheaper than parsing" and built a
recommendation on it. That was an open compared against a full parse: the same class of mistake
as the 3.9s further down, where work that was not done was counted as work made faster. A
partial cost is not a cost.

### What this means for TODO/28

`TODO/28` has two candidate implementations, and this measurement decides between them:

- **(a) a text harvest cache** — persist the extents and flags, re-slice, and call no libclang
  at all on the fast path. This is the only version that can approach zero, because it is the
  only one that does not walk 825728 cursors.
- **(b) reload the serialized AST and re-derive the harvest** — 1.21s instead of 1.95s per unit.
  Real, but small, and it still pays the 0.61s save on every cold split.

**(a) is the one to build.** Not because it is cleverer than libclang, but because the whole
point is to not need the AST, and (b) still needs it.

### And no, none of this is "reimplementing reparse"

Neither option updates an AST. Nobody should hand-patch one; that is what
`clang_reparseTranslationUnit()` is for, and where it is available it is the right tool. It is
simply not available here: a loaded TU refuses to reparse, and the live TU that would accept it
needs a process that outlives one launcher invocation.

(a) does not maintain an AST at all. It caches the *conclusions* the AST was consulted for —
which definitions exist, where they start and end, and where each belongs — and re-slices text
against them. That is a different and much smaller object than an AST, and it is the reason it
can be nearly free where reparse could not.

**If a persistent process is ever acceptable again, reparse beats both of these and should be
reconsidered before either.** The server was removed in `TODO/01` because when it was absent
the launcher silently compiled without splitting; that is a fixable defect — fail loudly — not
an argument against the process model. Worth revisiting deliberately rather than by accident.

## The preamble is not waste, and that was nearly missed

`split_unit()` parses with `CXTranslationUnit_PrecompiledPreamble |
CXTranslationUnit_CreatePreambleOnFirstParse`. Since nothing reparses, that looks like pure
waste, and it is expensive — **2.28s against 1.43s, a 60% surcharge on every parse**. Dropping
it took one unit's body-edit re-split from 12.2s to 8.3s.

**It is still not waste.** Both flags removed, two fixtures fail:

```
launcher.depfile_names_originals   no rewritten header in the dependency file
launcher.header_edit_behind_pch    second: expected '107', got '7'
```

`header_split_candidates()` builds its candidate list from `clang_getInclusions(tu, ...)`.
Parsed without a precompiled preamble — and with the prefix PCH supplying the include block
through `-include-pch` — that call yields nothing usable, `[auto-split]` never fires, and
**header splitting silently stops**. The build still succeeds and the objects still link; only
the fixtures notice.

### The false negative that nearly shipped it

Before running the fixtures the change was checked by re-splitting a Boost.Geometry unit with
each binary and comparing every output: **2935 files, all byte-identical.** That evidence was
worthless. Those headers had been split by an earlier run, so their manifests were current, and
`header_split_candidates()` skips a header whose manifest is current and merely registers it
(`TODO/25` defect 2). The expensive path under test was never entered.

A comparison on a warm tree cannot see a regression in work the warm tree skips.

## While measuring: "the parse" is not one parse

A body-edit re-split of that unit costs ~10s over the 2.2s cache-hit baseline, but a single
parse of it is 1.43-2.28s. The rest is the header candidates, each parsed standalone in the
same run, plus the prefix PCH, the emission, and compiling what changed. That is consistent
with the preamble's +0.85s per parse showing up as 3.9s once: the launcher parses the unit
*and* several headers.

Any statement that "the parse is the cost" should say which parse. `TODO/28`'s fast path has to
skip all of them or it saves a fifth of what it looks like it saves.

## The opportunity that remains

The preamble's only consumer is `clang_getInclusions()`, and 0.85s per parse is a large price
for a list of file names. The launcher already has that list elsewhere: it writes and parses a
depfile for the build system, and `depfile.cache` holds one per unit.

Not obviously safe: the depfile is written *after* the split, so a cold run has no list, and it
records what the compiler read rather than what libclang saw. Either the first run keeps the
preamble and later runs use the cache, or the candidate list comes from a preprocess-only pass.
Both are guesses until measured.

## Acceptance Criteria

If the preamble is ever dropped:

- `launcher.depfile_names_originals` and `launcher.header_edit_behind_pch` pass, and the reason
  they pass is that headers are still split — checked on a **cold** tree, with no header
  manifests present.
- `[auto-split]` fires for the same set of headers as today on a cold Boost.Geometry unit.
- The body-edit row improves by something close to the 32% measured here.
