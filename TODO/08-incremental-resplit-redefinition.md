# 08 — Second and later runs re-parse the source against its own preamble

**Severity:** High. The first split of a file works; every rebuild after it produces a
broken result, and deletes the split files the first run wrote.

**Found while testing TODO 05**, and confirmed present before that change.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

`build_libclang_pch()` builds a precompiled header from the generated preamble and passes
it as `-include-pch` when parsing the source. The preamble is the source with function
bodies removed -- it therefore contains every class, struct, enum and typedef the source
defines. Feeding it back in while parsing that same source redefines all of them:

```
Parse error: test/member_functions.cpp:8:  error: redefinition of 'Base'
Parse error: test/member_functions.cpp:13: error: redefinition of 'Widget'
Parse error: test/member_functions.cpp:41: error: redefinition of 'Holder'
Warning: 3 parse error(s) found. Output may be incomplete.
```

The first run is fine because no preamble exists yet, so no PCH is passed. From the second
run on, the parse is wrecked. The damage is not limited to a warning: the visitor then
finds almost nothing, and the stale-output pruning at the end of `emit_split_files()`
deletes everything the previous run wrote because it is no longer in the current file list:

```
1 function(s): 1 written, 0 unchanged, 12 stale removed
```

A 12-function file becomes a 1-function file. In launcher mode this means the first CMake
build splits correctly and every incremental build after it degrades, which is exactly the
workflow the compiler-launcher mode exists for.

## Description

Reproduce with any source that defines a type:

```sh
rm -rf /tmp/out
cpp-splitter test/member_functions.cpp /tmp/out --compile -o /tmp/bin   # works, 12 functions
cpp-splitter test/member_functions.cpp /tmp/out --compile -o /tmp/bin   # redefinition errors
```

The `#pragma once` at the top of the preamble does not help: the PCH is a separate entity
from the source file being parsed, so the guard never fires.

The PCH is meant to speed up parsing of the *split pieces*, each of which includes the
preamble and nothing else. It has no business being applied to the original source, whose
own text already provides those declarations.

### Implementation plan

1. Stop passing `-include-pch` when parsing the original source in `do_split()` and
   `do_split_with_cache()`. The preamble PCH is for compiling the split pieces, not for
   re-parsing their origin. This is the actual fix and is small.
2. Keep building the PCH -- the split pieces still benefit -- but move the `-include-pch`
   argument to the split-piece compile commands, where it is already implied by the
   `.gch`/`.pch` directory lookup, and verify it is not silently ignored there.
3. Make parse errors non-silent for the primary source too: a translation unit that failed
   to parse must not proceed to prune outputs. Gate the stale-output pruning on a clean
   parse, so a bad run can never delete a good run's work.
4. Add the guard regardless of the cause: pruning should only remove files for the unit
   being split, and only when that unit produced a plausible result (for example, refuse to
   prune everything when the current run found zero or near-zero functions).
5. Re-check the `CXTranslationUnit_PrecompiledPreamble` fallback path, which is used when
   no PCH is available -- that one is libclang-internal and is fine.

## Acceptance Criteria

- Running the splitter twice in a row over the same output directory produces identical
  output the second time: same number of functions, same file list, no `redefinition`
  parse errors, no `stale removed` count.
- `test/member_functions.cpp` and `test/static_specifier.cpp` both split, compile, link and
  run on the second and third consecutive run, not only the first.
- An incremental CMake rebuild of the Boost `filesystem` example (touch one source, rebuild)
  produces the same set of split files as the initial build.
- A parse failure never removes previously written split files.
- Regression test: split a file twice and diff the two output directories.

## Outcome

Implemented in `src/main.cpp`: the preamble's PCH is no longer passed as `-include-pch`
when parsing the original source, in either `do_split()` or `do_split_with_cache()`. Both
now always use `CXTranslationUnit_PrecompiledPreamble` /
`CXTranslationUnit_CreatePreambleOnFirstParse`, which is a different mechanism and is safe:
libclang caches the prefix of *this* file, so it cannot redefine anything the file declares.

Steps 3 and 4 of the plan -- never prune stale output after a failed parse -- were already
implemented as part of TODO 06, and the two fixes turn out to be complementary: TODO 06
stops the damage, this one stops the cause.

Verified:

| check | before | after |
|---|---|---|
| second run of the same file: `redefinition` errors | 3 | **0** |
| second run: split files produced | 13 (from 12) | **12, byte-identical** |
| `diff -rq` of first vs second output directory | differs | **no differences** |
| Boost incremental rebuild (touch one source): split file set | -- | **5067 files, identical to initial** |
| Boost incremental rebuild: `redefinition` errors | -- | **0** |

All three fixtures split, compile, link and run on consecutive runs, not just the first.

### Follow-up: the libclang PCH now has no consumer

`-include-pch` was the only thing that read the artifact `build_libclang_pch()` produces.
Split pieces are compiled by the real compiler, which auto-discovers the separate GCC-style
`<preamble>.gch/` PCH instead. So the libclang `.pch` is now built on every split and never
read.

Measured on the Boost `filesystem` build: **46.6s with it, 37.2s without** -- about 20% of
total build time spent producing an artifact nothing consumes.

The call is deliberately left in place rather than removed here, because `DOCS.md` presents
this PCH as a distributable artifact that enables distributed builds without a centralised
server, and dropping it is a design decision rather than part of this fix. Either it should
gain a consumer -- passing it to the split-piece compile commands, where it would be read
by the compiler rather than by libclang -- or it should be removed. This deserves its own
item.
