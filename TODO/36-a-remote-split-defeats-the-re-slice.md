# 36 — a split produced on the cluster defeats the next edit's re-slice

## Motivation

TODO/28 makes a one-line body edit cost one re-slice instead of a parse. TODO/35 moves the
split to the cluster. Measured together on Boost.Spirit's suite at `-j500`, Release, the second
destroys the first, and the splitter's own log says so plainly. Of the 268 units affected by
editing the body of `standard_wide::toucs4()`:

| the split those units already had | re-sliced here | re-split on the cluster | wall | remote actions |
|---|---:|---:|---:|---:|
| produced here | ~all | 0 | 75.5s | 1 |
| produced on the cluster | **1** | **267** | **606.5s** | **4835** |

A re-slice costs no parse and no network and rewrites one piece. A full split rewrites the
unit, so every piece gets a new action key and the cluster recompiles all of them.

## What has already been excluded

This entry previously claimed the cause was a missing prerequisite record, and was withdrawn
when that turned out to rest on a `find | wc -l` run against a live build tree. Both halves of
that history are now settled by measurement taken **after** the build finished:

- Every one of the 279 split directories holds a `depfile.cache` **and** an `inputs.hash`,
  whether the split came from the cluster or from here. The record is not missing.
- The mechanism that writes it survives a remote split, and it is worth knowing why: the
  launcher attaches `-MD -MF` to the unit's first *piece* compile, and the pieces are compiled
  here even when the split was not.
- A worker-side writer for `depfile.cache`, built from the include closure the parse walks, was
  implemented and reverted as redundant.
- It is not the double-listing defect either (`read_split_cache()` appending to a populated
  `SplitResult`): that is fixed, and this run had **0 fallbacks** on every row.

So the record exists, and the re-slice still does not happen. Something else about a
cluster-produced split makes `try_incremental_split()` decline.

## Implementation Proposal

Find out which of its preconditions fails, before changing anything. `try_incremental_split()`
declines for a small number of distinct reasons, and each is cheap to distinguish:

1. Make it say why. It currently returns `false` silently on every path; a one-line verbose
   note at each `return false` — no cache, no harvest, more than one changed prerequisite, the
   change was not confined to a recorded body — turns this into a one-run diagnosis rather than
   a guess. Do this first and re-run only the `one body` row.
2. The leading suspicion, to be confirmed or dropped by (1), is `changed_prerequisites()`
   reporting more than one changed file. `inputs.hash` is written from `depfile.cache`, which
   after a remote split is derived from a *piece* compile's `-MF` output rather than the unit's
   own — the two need not name the same set, and if the recorded set does not match what the
   next run computes, every prerequisite looks changed.
3. Fix what (1) names, and keep the local-split rows unchanged.

## Acceptance Criteria

- `launcher.remote_split_on_opal` already asserts the invariant at fixture scale — split on the
  cluster into an empty tree, edit one body, require a local re-slice and no cluster round trip
  — and it passes today. It therefore does **not** reproduce this, and reproducing it is part of
  the work: extend it until it fails for the same reason Spirit does, or explain what Spirit has
  that two units do not.
- Boost.Spirit, `one body`, split on the cluster: the splitter's log reports the re-slice for
  every affected unit and a full split for none, and the row's remote action count comes back to
  the order of 1 rather than 4835.
- The `full` row is unaffected, and no row gains a fallback.
