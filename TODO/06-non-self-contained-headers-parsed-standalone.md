# 06 — Non-self-contained headers are parsed as standalone translation units

**Severity:** Medium. Produces parse errors and, worse, continues past them to emit
output derived from an incomplete AST.

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
