# 06 — Non-self-contained headers are parsed as standalone translation units

**Severity:** Medium. Produces parse errors and, worse, continues past them to emit
output derived from an incomplete AST.

**Status: Step 1 implemented and verified. Step 2 not started.** See "Outcome" below.

## Motivation

Automatic header splitting feeds every `#include`d project header to libclang as its own
translation unit. Many real headers are not self-contained by design and cannot be
parsed alone: prefix/suffix pairs, "push/pop" headers, and headers that require a macro
context established by their includer.

From the Boost `filesystem` run:

```
[auto-split] .../libs/config/include/boost/config/abi_prefix.hpp
No function definitions found in .../abi_prefix.hpp

[auto-split] .../libs/filesystem/include/boost/filesystem/detail/footer.hpp
Parse error: .../boost/config/abi_suffix.hpp:13: error: Header boost/config/abi_suffix.hpp
             must only be used after boost/config/abi_prefix.hpp
Warning: 1 parse error(s) found. Output may be incomplete.
No function definitions found in .../footer.hpp
```

Boost's `detail/header.hpp` / `detail/footer.hpp` and `config/abi_prefix.hpp` /
`config/abi_suffix.hpp` are deliberately unbalanced halves of a pair — each `#error`s or
misbehaves when parsed without its partner. This pattern is common well beyond Boost
(ABI push/pop, warning push/pop, `_BEGIN_NAMESPACE`-style headers).

The immediate cost here is noise. The real cost is that the tool prints
`Output may be incomplete` and **carries on**, emitting split files and a rewritten
header copy built from a partial AST. Combined with TODO 04 (the rewritten copy going on
the include path), a partially-parsed header can displace a correct one.

## Description

The auto-split path parses each discovered header independently and treats parse errors
as a warning rather than a disqualification. The parent translation unit — which
included the header in its correct context and has already been parsed successfully — is
discarded for this purpose.

### Implementation plan

Two changes, the first a safety fix and the second the real solution.

**Step 1 — fail closed (small, do this first).**

1. Treat any parse error while splitting a header as fatal *for that header*: skip it
   entirely, do not emit split pieces, do not emit a rewritten copy, and do not register
   it in the manifest.
2. Record the header as non-splittable in the manifest (with a reason) so the decision is
   cached and is visible when diagnosing a build.
3. Keep the parent translation unit's compilation working by simply leaving that header
   alone — it will be included normally from its original location.
4. Downgrade the current `Warning: ... Output may be incomplete` to an explicit
   `Skipping <header>: not parseable standalone (<first error>)` at normal verbosity.

**Step 2 — harvest from the parent TU instead of re-parsing.**

5. The parent `CXTranslationUnit` already contains every function defined in every
   included header, parsed in the correct macro and inclusion context. Walk the parent
   AST once and bucket function cursors by their defining `CXFile`
   (`clang_getCursorLocation` → `clang_getFileLocation`) instead of re-parsing each
   header standalone. This removes the standalone-parseability requirement entirely, and
   also removes N redundant header parses per translation unit — a significant part of
   the 32.9s (vs 0.7s baseline) that the Boost build currently costs.
6. Preamble generation for a header still needs the header's own source text, which is
   available independently of the AST; only the function inventory needs to come from the
   parent TU.
7. Note the interaction with the existing per-header `.pch` caching: harvesting from the
   parent TU changes what can be cached and shared. Re-evaluate whether per-header PCHs
   still pay for themselves once redundant parses are gone.
8. Retain a small denylist for headers that should never be split even when harvesting
   succeeds (`abi_prefix.hpp`, `abi_suffix.hpp`, `detail/header.hpp`, `detail/footer.hpp`
   and anything whose text contains a bare `#error` outside an include guard), since
   emitting a rewritten copy of an unbalanced half-header is never correct.

## Acceptance Criteria

- A Boost `filesystem` split build logs no `Output may be incomplete` warnings.
- `abi_prefix.hpp`, `abi_suffix.hpp`, `detail/header.hpp` and `detail/footer.hpp` are
  reported as skipped, and no rewritten copies or split pieces are emitted for them.
- Headers that fail to parse are never registered in a `.split` manifest as splittable.
- After Step 2: the number of libclang parses per translation unit is 1 (plus PCH
  loads), verified under `CPP_SPLITTER_VERBOSE=1`; wall time for the Boost `filesystem`
  example drops measurably against the 32.9s baseline recorded before this work.
- Function inventories harvested from the parent TU match, for a self-contained header,
  the inventory produced by the current standalone parse.
- Regression fixture: a header pair where the second `#error`s unless the first was
  included, consumed by one translation unit; the build must succeed with both headers
  skipped.

## Outcome

**Step 1 (fail closed) is done. Step 2 (harvest from the parent TU) is not**, and is left
open -- it is a rearchitecture of how headers are discovered, not a fix to this behaviour,
and it carries the performance work with it.

Implemented in `src/main.cpp`:

- `header_has_include_guard()` decides the denylist generically instead of by name. A
  header meant to be included once carries `#pragma once` or a classic `#ifndef X` /
  `#define X` pair; one designed for repeated inclusion -- half of a header/footer or
  push/pop pair -- carries neither, and rewriting half a pair is never correct. This
  catches `detail/header.hpp`, `detail/footer.hpp` and `iterator/detail/config_undef.hpp`
  without hardcoding them, and is checked before parsing, so those headers cost nothing.
- A header whose standalone parse reports errors is now skipped outright: no split pieces,
  no rewritten copy, no manifest entry. This catches `abi_prefix.hpp` / `abi_suffix.hpp`
  (which `#error` when included without their partner) and the Boost.MPL preprocessed
  headers.
- `write_skipped_header_manifest()` caches the decision with the reason, so it is made once
  rather than on every invocation, and `load_header_manifests()` will not register it
  (an empty compilable list means "nothing to link").
- The `Warning: N parse error(s) found. Output may be incomplete.` line is gone.
  `check_diagnostics()` no longer editorialises; each caller says what it actually did --
  `Skipping <header>: ...` for a skipped header, or an explicit
  `splitting anyway, and no stale output will be pruned` for a source.
- Stale-output pruning is now gated on a clean parse. A run that saw almost no functions
  because the parse failed can no longer delete a good run's work.

Verified on the Boost `filesystem` build:

| check | before | after |
|---|---|---|
| `Output may be incomplete` warnings | 73 | **0** |
| rewritten copies of `abi_prefix` / `abi_suffix` / `header` / `footer` | emitted | **0** |
| total `error:` lines | 2804 | **1263** |
| headers skipped, decision cached with a reason | n/a | 314 |

The three fixtures still split, compile, link and run.

### What Step 2 would still buy

Unchanged by Step 1: 2 of 12 translation units link from split objects, and the same four
fail. Their remaining errors -- `no member named`, `constexpr variable`,
`non-constexpr declaration of`, `constructor cannot have a return type` -- are missing
declaration context in headers that *do* parse standalone but not the way their includer
sees them. Only harvesting from the parent translation unit fixes that.

The redundant parses are also still there: 2769 `[auto-split]` header parses across 12
translation units. The Step 2 acceptance criteria (one libclang parse per translation unit,
and a measurable wall-time drop) are therefore not met.
