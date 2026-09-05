# 14 — Re-parsing, not re-compiling, is what an incremental split build spends its time on

**Severity:** High for the tool's purpose. Splitting exists to make an edit cheaper to
rebuild, and today it makes every edit more expensive. This is the item that decides whether
the approach pays for itself.

## Motivation

`./benchmark-boost-split.sh` compares Boost.Filesystem built through the splitter against
the same tree built normally, identical apart from `CMAKE_CXX_COMPILER_LAUNCHER`:

| scenario | plain | split | ratio |
|---|---:|---:|---:|
| full | 0.7s | 16.3s | 24.2x |
| no-op | 0.2s | 0.2s | 1.0x |
| one source | 0.5s | 1.0s | 2.1x |
| one header | 0.7s | 1.7s | 2.5x |
| one function body in a header | 0.7s | 2.5s | **3.6x** |

The last row is the scenario the whole design is aimed at -- change one inline function in a
widely included header -- and it is the one the splitter loses by the widest margin.

The interesting part is where that time goes. Instrumenting the same edit:

```
translation units rebuilt:      3
pieces recompiled:              0
pieces found up to date:       61
```

**The piece-level machinery works exactly as intended.** The splitter recognised that no
piece's text had changed and compiled nothing at all. Every second of that 2.5s went on
re-parsing the three affected translation units with libclang and regenerating output that
turned out to be byte-for-byte identical.

So the compile-avoidance half of the idea is already delivered, and it is bought at a price
that exceeds what it saves. Splitting is only worthwhile if analysing a translation unit is
cheaper than compiling it, and right now it is not.

## Reproduction

```sh
./benchmark-boost-split.sh
```

To see the breakdown rather than the total, edit the body of `is_regular_file` in
`libs/filesystem/include/boost/filesystem/operations.hpp` and rebuild with
`CPP_SPLITTER_VERBOSE=1`, then count `up-to-date:` against `compile (` in the log.

## Description

Every launcher invocation parses its translation unit from scratch. For Boost.Filesystem
that is a Boost-sized include graph per unit, and it happens on a full build, on an
incremental build, and even when the resulting pieces are identical to the ones already on
disk.

There was a mechanism intended to amortise this -- a precompiled header built from the
preamble and fed back via `-include-pch`. TODO 08 had to remove it: the preamble is the
source with its function bodies carved out, so it declares everything the source declares,
and feeding its PCH back into a parse of that same source redefined all of it. The artefact
is still built on every split and now has no consumer at all, at a measured cost of roughly
a fifth of total build time.

### The proposal, simplest first

**1. A prefix PCH.** The reason the preamble PCH could not be fed back is that it contained
the source's own declarations. A precompiled header built from *only the source's include
directives* -- its prefix, up to the first non-preprocessor construct -- has no such
problem: it declares nothing the source declares, so including it is exactly what the
compiler would have done anyway, only already parsed.

That is where the time is. The include graph is what makes these parses expensive; the
source's own few hundred lines are not. It is also stable in a way the preamble is not: it
changes only when the include list changes, so it survives ordinary edits and is shared by
every translation unit with the same prefix.

Content-hash naming and cleanup already exist for the current PCH and can be reused as they
stand.

**2. Skip the split when nothing it depends on changed.** Cheaper still, and complementary.
The split of a translation unit is a pure function of the source, the flags and the contents
of every file it includes -- and since TODO 12 that dependency list is already written next
to the pieces as `depfile.cache`. Hash those inputs, store the hash beside the output, and
when it matches, skip the parse entirely and reuse what is on disk.

This does not help the benchmark's worst row, where the header genuinely did change, but it
makes a no-op or unrelated edit free rather than merely fast, which is the common case in a
real edit-build loop.

**3. Only then consider the server.** A persistent process holding parsed translation units
in memory is the obvious answer and is already half-built, but it is also the reason
`CPP_SPLITTER_NO_SERVER=1` is currently mandatory (TODO 01) and it does not survive a
distributed build. A file-based prefix PCH plus input hashing gets most of the benefit
without a daemon, and both work when the build is farmed out to other machines.

## Acceptance Criteria

- The `one function body in a header` row is faster with the splitter than without, on the
  Boost example. That is the scenario the design exists for and the one to judge it by.
- A no-op build performs no libclang parse at all, verifiable by counting parses under
  `CPP_SPLITTER_VERBOSE=1`.
- An edit to a file that a translation unit does not include leaves that unit's split
  untouched, with no parse.
- The full build is no slower than it is today, and preferably faster once the unused PCH
  stops being written.
- `libclang PCH built:` no longer appears for an artefact nothing reads; either it has a
  consumer or it is gone.
- The library produced is byte-identical to one produced without any caching, checked by
  building twice with the cache cleared in between.
