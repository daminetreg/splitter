# 18 — Two definitions from one macro can report overlapping, unequal extents

**Severity:** High. 18 of the 62 fallbacks in the wider Boost run, all of them one macro in
one header: `BOOST_STRONG_TYPEDEF` in `boost/serialization/serialization.hpp`.

## Motivation

TODO 16 established that several definitions can share one source extent -- the macro
invocation they came from -- and that `generate_preamble()` collapses identical extents so
the invocation is not written out once per definition. What it did not establish is that the
extents are not always *identical*. They can overlap.

A macro argument is spelled in the invocation, not in the macro body. When a definition's
first token comes from an argument, libclang maps its extent start to where the argument was
written -- inside the invocation, not at its beginning. So one macro produces definitions
with extents `[start-of-invocation, end]` and `[start-of-argument, end]`, which overlap and
compare unequal.

`std::unique` collapses only exact duplicates, and the emit loop then re-emits the tail of
the invocation:

```cpp
BOOST_STRONG_TYPEDEF(unsigned int, version_type)
version_type)
```

## Reproduction

```cpp
// b.hpp
#pragma once
#define ST(T, D)                                   \
struct D {                                         \
    T t;                                           \
    explicit D(const T& v) : t(v) {}               \
    D& assign(const T& r) { t = r; return *this; } \
};
ST(unsigned int, version_type)
```

```cpp
// b.cpp
#include "b.hpp"
int main() { version_type a(2); a.assign(3u); return a.t == 3u ? 0 : 1; }
```

```
$ cpp-splitter clang++ -I. -c -o b.o b.cpp
b.o.split/include/b.hpp:10:13: error: expected unqualified-id
[cpp-splitter] split build failed, falling back to normal compilation

$ tail -2 b.o.split/include/b.hpp
ST(unsigned int, version_type)
version_type)
```

`assign` is the member that matters: its declaration begins with `D&`, and `D` is a macro
argument, so its extent starts at `version_type` inside the invocation rather than at `ST`.
The constructor's extent starts at `ST`. `CPP_SPLITTER_DUMP_HARVEST=b.hpp` shows it: the two
definitions report `shared=0`, meaning neither found an exact twin.

Remove `assign`, or give it a return type that is not a macro argument, and the two extents
become identical, the collapse works, and the file splits.

## Description

In `generate_preamble()` (`src/main.cpp:1472`):

```cpp
ranges.erase(std::unique(ranges.begin(), ranges.end(),
                         [](const Range& a, const Range& b) {
                             return a.start == b.start && a.end == b.end;
                         }),
             ranges.end());
```

and then, per range, `if (r.start > pos) preamble += source.substr(pos, r.start - pos);`
followed by the kept text `source.substr(r.start, r.end - r.start)`.

For an overlapping second range the gap is empty -- correctly, `r.start < pos` -- but the
kept text is emitted anyway, duplicating the bytes from `r.start` to `pos`. If the second
range were a *split* one rather than a kept one, the outcome is worse than duplication: its
declaration would be written in the middle of text that has already been emitted, and the
definition it was supposed to remove would still be there.

`shares_extent` in `prepare_functions()` has the same exact-equality assumption and so does
not keep these definitions for the right reason -- they happen to be kept anyway, because a
macro invocation has no `{` and `definition_decl_end()` finds no body to move.

## Implementation spec

1. **Group by overlap, not by equality.** In `prepare_functions()`, replace the
   `extent_uses` map with an interval sweep over the extent-sorted definitions: a definition
   overlaps its predecessor when `start < running_max_end`. Every member of an overlapping
   run gets `shares_extent = true`, and the run's union `[min_start, max_end)` is recorded on
   each of them as the group extent.

2. **Emit the union once.** In `generate_preamble()`, replace `std::unique` with a merge that
   folds an overlapping run into a single `Range` spanning the union, `keep = true`, `fn =
   nullptr`. A run is by construction unsplittable: no one declarator covers it, so there is
   no declaration to leave behind and nothing to move. Emitting the union verbatim reproduces
   the source exactly, which is what the current identical-extent collapse achieves for the
   easy case.

3. **Keep the invariant explicit.** After the merge, assert (or, in release, bail to
   `keep_in_header` for the whole file) that the ranges are strictly increasing and
   non-overlapping. The emit loop's correctness depends on that and nothing currently states
   it.

`keep_reason()` already has a `shares_extent` case; widening the rule to overlap makes the
reason it reports true rather than accidentally true.

## Acceptance Criteria

- The reproduction above splits without falling back, and the program returns 0.
- A regression fixture in `test/`: a macro producing at least two members, one of whose
  declarations begins with a macro argument, so the extents overlap without being equal.
- No generated header contains text repeated from a macro invocation. Diffing the rewritten
  header against the original outside function extents shows only removals, never
  insertions.
- The wider Boost run loses the 18 fallbacks in this class.
