# 29 — Why `clang_reparseTranslationUnit` does not apply, and what the preamble is really for

**Severity:** None on its own. This is the design note that keeps `TODO/28` from being
attempted the easy-looking way, plus one measured opportunity it uncovered.

## The question

`TODO/28` wants a body-only edit to skip the parse. libclang has an API built for exactly
that: `clang_reparseTranslationUnit()`, which an editor calls after every keystroke. Why not
use it?

## Why it does not apply

**1. There is no translation unit to reparse.** `clang_reparseTranslationUnit()` takes a live
`CXTranslationUnit` produced by `clang_parseTranslationUnit()` *in the same process*. The
launcher is one shot per translation unit: ninja runs it, it splits, it exits. The server that
could have held state was removed in `TODO/01`, deliberately, because its absence silently
disabled splitting. A TU written out with `clang_saveTranslationUnit()` and read back with
`clang_createTranslationUnit()` is not a substitute -- a loaded TU is not reparsable.

Restoring a persistent process to get this back would reintroduce `TODO/01`'s failure mode as
the price of an optimisation, which is the wrong trade.

**2. Even with a live TU, the edit we care about invalidates the thing that makes reparse
fast.** Reparse is cheap only because of the precompiled preamble: the leading run of
`#include`s is compiled once and reused, so only the text below them is re-parsed. Any change
to a file *inside* that preamble discards it and it is rebuilt.

The row `TODO/28` is about is a **header** edit -- `side_info.hpp`, reached through the include
block. It is inside the preamble by construction. Reparse would rebuild the preamble and save
nothing.

Reparse helps when the main file changes below its own includes. That is the `one source`
scenario, which the split cache already wins 3.3x on without any of this machinery.

So `TODO/28` stands as specified: cache the harvest and re-slice it. The saving has to come
from not needing the AST, not from getting the AST more cheaply.

## What the question uncovered

`split_unit()` parses with `CXTranslationUnit_PrecompiledPreamble |
CXTranslationUnit_CreatePreambleOnFirstParse`. Since nothing ever reparses, that looks like
pure waste, and dropping it is worth a lot:

| `boost_geometry_algorithms_area`, body edit | wall |
|---|---:|
| with the preamble flags | 12.2s |
| without them | **8.3s** |

**It is not waste.** Both flags removed, two fixtures fail:

```
launcher.depfile_names_originals   no rewritten header in the dependency file
launcher.header_edit_behind_pch    second: expected '107', got '7'
```

`header_split_candidates()` builds its candidate list from `clang_getInclusions(tu, ...)`.
Parsed without a precompiled preamble -- and with the prefix PCH supplying the include block
through `-include-pch` -- that call yields nothing usable, `[auto-split]` never fires, and
**header splitting silently stops**. The build still succeeds and the objects still link; only
the fixtures notice.

### The false negative that nearly shipped it

Before running the fixtures this change was checked by re-splitting a Boost.Geometry unit with
each binary and comparing every output: **2935 files, all byte-identical.** That evidence was
worthless. Those headers had been split by an earlier run, so their manifests were current,
and `header_split_candidates()` skips a header whose manifest is current and merely registers
it (`TODO/25` defect 2). The expensive path under test was never entered.

A comparison run on a warm tree cannot see a regression in work the warm tree skips. The
fixtures caught what a 2935-file diff could not.

## The opportunity that remains

3.9 seconds of a 12.2-second re-split -- 32% -- is spent building and serialising a preamble
whose only consumer is `clang_getInclusions()`. That is a large price for a list of file
names, and the launcher already has that list from another source: it writes and parses a
depfile for the build system, and `depfile.cache` holds one per unit.

Worth investigating, and not obviously safe: the depfile is written *after* the split, so a
cold run would have no list to work from, and it records what the compiler read rather than
what libclang saw. Either the first run keeps the preamble and later runs use the cache, or
the candidate list comes from a cheap preprocess-only pass. Both are guesses until measured.

## Acceptance Criteria

If the preamble is ever dropped:

- `launcher.depfile_names_originals` and `launcher.header_edit_behind_pch` pass, and the
  reason they passed is that headers are still split -- checked on a **cold** tree, with no
  header manifests present.
- `[auto-split]` fires for the same set of headers as today on a cold Boost.Geometry unit.
- The body-edit row improves by something close to the 32% measured here.
