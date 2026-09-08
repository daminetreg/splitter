# 29 — Reparse, the serialized AST, and the preamble that is not waste

**Severity:** None on its own. This is the design note behind `TODO/28`, and the reason its
saving has to come from one particular direction.

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
| **`clang_createTranslationUnit2` (load the `.ast`)** | **0.02s** |
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

### 3. The useful part: loading a serialized AST is 70x cheaper than parsing

**0.02s against 1.43s.** That is the fact worth having, and it is better than reparse for
`TODO/28`'s purpose, because a body-only edit does not need a *new* AST at all — it needs the
*old* harvest, and the old AST is a complete record of it.

So `TODO/28` has two candidate implementations rather than one:

- **(a) a text harvest cache** — write the extents and flags to `<tag>.harvest` and re-slice.
  Nothing but file I/O, no libclang in the fast path, and the format is inspectable.
- **(b) a serialized AST** — keep the `.ast` and re-derive the harvest by walking it at 0.02s.
  No new format to design or version, and `collect_emitted()` can be re-run exactly rather
  than approximated, which is the one decision `TODO/28` flags as genuinely at risk.

(b) trades disk for correctness and is the more interesting of the two. Against it: the `.ast`
files are large, `clang_saveTranslationUnit` costs 0.61s on every cold split, and the AST
describes the source *as it was*, so the edited body's new text still has to come from the file
and the extents after it still have to be shifted. Neither approach escapes that arithmetic.

Both need measuring on a real unit before either is written.

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
