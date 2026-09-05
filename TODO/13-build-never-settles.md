# 13 — One translation unit rebuilds on every invocation

**Severity:** Medium on its own -- a few seconds per build -- but it masks TODO 12, because
a build that always has something to do looks like a build that is responding to changes.

**Status: implemented.** See "Outcome" at the end.

## Motivation

A build with nothing to do should do nothing. This one always rebuilds
`libs/atomic/src/lock_pool.cpp.o` and relinks `libboost_atomic.a`:

```
run 1 rebuilt: src/lock_pool.cpp.o  link
run 2 rebuilt: src/lock_pool.cpp.o  link
run 3 rebuilt: src/lock_pool.cpp.o  link
```

Found while confirming TODO 12, and worth separating from it: while investigating that
issue, every `ninja` invocation appeared to rebuild *something*, which made it much harder
to tell whether an edit had been noticed. Two objects rebuilding after touching a header
looked at first like the header had been detected, when in fact it was this.

## Reproduction

```sh
cd /tmp/boost-split-repro
CPP_SPLITTER_NO_SERVER=1 ninja      # settle
CPP_SPLITTER_NO_SERVER=1 ninja      # expected: no work. actual: rebuilds lock_pool
```

`lock_pool.cpp` is the only translation unit that does this on the Boost example, which
suggests something specific to it rather than a general property of the pipeline.

## Description

Diagnosed: the object is older than its own source. The shape of the problem is that one of the object's recorded
prerequisites is newer than the object every time, so the candidates are:

- a generated file that the splitter rewrites on every run even when the input has not
  changed -- the preamble, the definitions header, or one of the rewritten headers -- so its
  timestamp always moves;
- a prerequisite that is written *after* the object it is a prerequisite of, so the ordering is
  wrong regardless of content;
- a prerequisite recorded under two different spellings of the same path, so one of them
  never matches.

`lock_pool.cpp` is the translation unit with the largest header fan-out on this example and
the one that pulls in `boost/align` and the SSE detection headers, which is where a
generated-file-per-run would show up first.

### Implementation plan

1. Find which prerequisite is dirty: `ninja -d explain` names the file it thinks changed,
   which should identify the mechanism immediately.
2. If it is a generated file being rewritten unconditionally, make the write conditional on
   the content differing. The preamble already does this -- it compares against the existing
   file and skips the write when identical -- so whichever artefact is at fault is missing
   that check. Candidates are the definitions header added for TODO 10, the rewritten header
   copies, and the `.split` manifests.
3. If it is an ordering problem, ensure every generated prerequisite is written before the
   object that depends on it, not during the same step.
4. Add a settle check to the test suite: build twice, assert the second run reports no work.
   That is a cheap invariant and would have caught this immediately.

## Acceptance Criteria

- A second `ninja` on a settled Boost build reports no work at all.
- The same holds after an incremental rebuild triggered by a real edit: build, edit, build,
  build -- the last one is a no-op.
- The settle check is part of the test suite, so a regression shows up without anyone
  watching build logs.

## Outcome

`ninja -d explain` named it directly:

```
output libs/atomic/CMakeFiles/boost_atomic.dir/src/lock_pool.cpp.o older than most recent
input .../libs/atomic/src/lock_pool.cpp
```

When every piece is already current the launcher skips the relocatable link, which is the
right thing to do -- nothing changed the object's contents. But the build system decides
staleness from modification time, so an object left older than an input it no longer differs
from is judged dirty on every invocation, for ever. The skip branch now touches the object,
recording that this build considered it and found it current.

Two further gaps in dependency reporting had to be closed before the build actually settled,
both belonging to TODO 12 and fixed with it: the dependency file was written only when
something recompiled, so a second build discarded what the first had learned; and a
translation unit whose whole body arrives through an included file has no pieces of its own,
so the `-MD` flags went nowhere and it recorded no dependencies at all. Dependency flags are
now attached to the first header piece when there is no own piece to carry them.

Verified on the Boost example:

| check | before | after |
|---|---|---|
| targets rebuilt by a second build of a settled tree | 2, for ever | **0** |
| dependency caches written | -- | **12 of 12** |
| dependencies recorded for `utf8_codecvt_facet.cpp.o` | 0 | **226** |
| editing `utf8_codecvt_facet.ipp` rebuilds its unit | no | **yes** |

### The underlying point

This is a workaround for timestamps being the wrong signal. The object's contents did not
change, and the honest answer is that nothing needed doing; touching it is a way of saying
so in the only vocabulary the build system has. A build system that compared content rather
than modification time would need none of this, and would also stop rebuilding everything
downstream when the splitter regenerates a piece byte-for-byte identically -- which is the
common case, since a piece is only rewritten when its text actually differs.

### A note on measurement

The first attempt appeared to make the build rebuild *more*, and several minutes went into
chasing that. The cause was that the disk had filled -- each split build of this example is
about 1.6 GB, and repeated builds had taken the filesystem to 100% -- so the dependency
caches were silently failing to write. Worth knowing when reproducing: check `df` before
concluding anything from a build that behaves strangely.
