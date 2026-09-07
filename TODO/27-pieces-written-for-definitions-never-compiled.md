# 27 — A piece is written for every definition, including the ones never compiled

**Severity:** Medium, and it is pure waste rather than a defect: the output is correct, it is
just mostly unread. It is also a large part of what `TODO/14` still has not met.

## Motivation

A definition that cannot be split is kept in the preamble — a function template, a member of a
class template, a virtual, a constructor in a header. `prepare_functions()` decides that, and
it is the right decision: there is nothing to gain from splitting a template out, because a
template is not instantiated until someone uses it and no object file can hold that work in
advance.

`emit_split_files()` writes the piece anyway. It carries a note saying why it will not be
compiled:

```cpp
// Function: bool boost::algorithm::all_of(const Range &, Predicate)
// Source: .../boost/algorithm/cxx11/all_of.hpp (lines 46-50)
// Note: kept in the preamble, not compiled -- function template
```

and no object is ever produced from it.

Measured on one Boost.Geometry test translation unit
(`libs/geometry/test/algorithms/area/area_multi.cpp`):

| | count |
|---|---:|
| pieces written | **11515** |
| pieces compiled | **273** |

**97.6% of the files written are never compiled**, and they are 12 MB of `.cpp` for that one
translation unit. There are 24 test targets.

By reason:

| | |
|---:|---|
| 6079 | function template |
| 2588 | member of a class template |
| 1597 | shares its source extent with another definition |
| 296 | not emitted by this translation unit |
| 222 | constructor or destructor in a header |
| 215 | virtual member function |

Templates and their members alone are 8667 of the 11242 wasted writes.

## Why it does this

The comment in `emit_split_files()` gives the reason: the piece is written "so that the file
list stays stable", and the `Note:` line tells whoever opens it which of the many keep rules
applied. That second half has real value -- several of this month's defects were diagnosed by
opening one of these files and reading the reason.

The first half no longer holds. Piece names are `<tag>_<counter>_<name>.cpp` and the counter
advances for every definition whether or not a file is written, so the names of the pieces that
*are* compiled do not move. And the pruner already deletes any `.cpp` in the unit directory
that the current run did not produce, so files left over from before are cleaned up rather than
stranded.

## What it costs

`TODO/14`'s outcome names this as part of what its acceptance criterion still misses:

> What is left is the rest of the split: walking the AST twice, once to harvest and once for
> `collect_emitted()`, and writing several hundred output files per unit whose contents usually
> turn out to be unchanged.

"Several hundred" was the filesystem example. On Boost.Geometry it is eleven thousand per
translation unit, and it is paid on every full build and on every edit that invalidates the
split cache.

## Implementation spec

1. **Do not write a piece for a kept definition.** In `emit_split_files()`, skip the file
   entirely when `should_keep_in_header(fn)`. Keep advancing `file_counter`, so the names of
   compiled pieces are unchanged and incremental builds do not see everything move.

2. **Keep the diagnosis, once per unit.** Write `<tag>.keeps` beside the pieces: one line per
   kept definition, `<counter>\t<line>\t<reason>\t<signature>`. That is the same information
   the `Note:` lines carried, in one file instead of eleven thousand, and it can be read
   without knowing which piece number to look in.

3. **Leave a way back.** `CPP_SPLITTER_DUMP_HARVEST` is already the knob for this kind of
   detail. When it is set, write the pieces as before -- the per-file form is easier to read
   when the question is "what exactly would this have compiled".

4. **Let the pruner do its job.** Kept pieces from an earlier run are not in `current_files`,
   so the existing pruning loop removes them and their stale objects. No new deletion code.

5. **Do not change `compilable_files`.** It never contained kept pieces, so the split cache,
   the link, and the depfile machinery are unaffected.

## Acceptance Criteria

- The Boost.Geometry translation unit above writes about as many pieces as it compiles, plus
  one `.keeps` file, rather than 42x as many.
- `<tag>.keeps` names every definition that was kept, with the reason `keep_reason()` gives.
- `CPP_SPLITTER_DUMP_HARVEST=<file>` still writes the individual pieces.
- A tree built before this change, rebuilt after it, is left with no stale kept pieces.
- The test suite, the four-library harness and the filesystem example are unchanged: same
  fallbacks, same 9/9.
