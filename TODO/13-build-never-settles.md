# 13 — One translation unit rebuilds on every invocation

**Severity:** Medium on its own -- a few seconds per build -- but it masks TODO 12, because
a build that always has something to do looks like a build that is responding to changes.

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

Not yet diagnosed. The shape of the problem is that one of the object's recorded
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
