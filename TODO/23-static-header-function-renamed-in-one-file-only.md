# 23 — A `static` function in a header is renamed, but only its own header learns the name

**Severity:** Medium. 1 of the 62 fallbacks in the wider Boost run
(`thread/src/pthread/thread.cpp`), but the mechanism reaches any header pair where one
defines an internal-linkage function and another calls it.

## Motivation

A split-out definition with internal linkage has to become external, or no other piece can
call it, and it has to be renamed, or objects from different translation units collide when
`ld -r` merges them. `build_static_rename_map()` does that, and
`apply_static_renames()` rewrites the name everywhere -- in the preamble text, the
declarations and the split bodies.

Everywhere in *one file*. The map is built per split unit from that unit's own functions
(`src/main.cpp:866`, called at `2285` and `2520`), and each header is split into its own
preamble. So when `boost/thread/detail/platform_time.hpp` defines

```cpp
static inline platform_duration platform_milliseconds(long const& ms)
{
  return platform_duration(ms * 1000000l);
}
```

its rewritten header says

```cpp
platform_duration __static_platform_time_hpp__platform_milliseconds(long const& ms);
```

while `boost/thread/pthread/mutex.hpp`, which was rewritten separately, still says

```cpp
d = (std::min)(d, detail::platform_milliseconds(BOOST_THREAD_POLL_INTERVAL_MILLISECONDS));
```

```
mutex.hpp:265:39: error: no member named 'platform_milliseconds' in namespace 'boost::detail'
```

## Reproduction

```cpp
// f_def.hpp
#pragma once
namespace demo { static inline int scale(int n) { return n * 2; } }
```

```cpp
// f_use.hpp
#pragma once
#include "f_def.hpp"
namespace demo { inline int scaled_plus(int n) { return scale(n) + 1; } }
```

```cpp
// f.cpp
#include "f_use.hpp"
int main() { return demo::scaled_plus(3) == 7 ? 0 : 1; }
```

```
$ cpp-splitter clang++ -I. -c -o f.o f.cpp
f_use.hpp:3:40: error: use of undeclared identifier 'scale'
[cpp-splitter] split build failed, falling back to normal compilation
```

Two headers are essential. Put both functions in one header and the rename map covers both
uses, and it works.

## Description

The rename exists for a real reason, and TODO 03 already records one round of it: a name that
changes has to change at every use, including uses carried verbatim into the preamble. What
was not considered is that a translation unit is split into *many* preambles -- one per
header -- and the map is scoped to whichever one is being written.

There are two ways out, and they trade splitting against simplicity.

**A. Keep internal-linkage definitions that live in headers.** This is the argument
`prepare_functions()` already makes for unnamed namespaces:

> Keeping the definition in the preamble costs a copy per object, which internal linkage
> makes harmless, and keeps the meaning of the code intact.

A `static` function at namespace scope in a header is the same situation: every translation
unit that includes the header already gets its own copy, so a copy per piece changes nothing
about the program's meaning. `in_unnamed_ns` is in fact a subset of this case --
`clang_getCursorLinkage()` reports `CXLinkage_Internal` for both -- so the existing rule is
half of the proposed one.

The cost is that these are not split. In `platform_time.hpp` that is nine small functions.

**B. Build the rename map for the whole translation unit.** Rename every internal-linkage
definition once, keyed by the file it was defined in, and hand the same map to every header's
`generate_preamble()`. This splits more, and it is the shape the rename was reaching for.

It is not free. Headers are split once and shared between translation units via
`g_split_headers`, and `unit_tag` -- which seeds the mangled name -- differs per unit. Two
units that include `platform_time.hpp` would have to agree on the mangled name or the shared
split output is not shareable; keying the mangle on the *defining header's* path rather than
on the including unit's tag is what makes them agree, and would need checking against the
basename-collision rule in TODO 04.

**Recommendation: A now, B if the lost splitting is ever measured to matter.** A is four
lines and provably correct; B is a change to how split headers are shared, and the fallbacks
it would recover are one translation unit in this corpus.

## Implementation spec (option A)

1. In `prepare_functions()`, alongside the existing `in_unnamed_ns` clause, keep any
   definition for which `fn.is_static` is true and `is_included_file(fn.file)` is true --
   that is, internal linkage in a header or implementation include rather than in the
   translation unit's own source.

2. Add a `keep_reason()` case: `"internal linkage in a header"`. It must come before the
   `in_unnamed_ns` case only if it should win the reporting; put it after, so an unnamed
   namespace still reports the more specific reason.

3. `build_static_rename_map()` then never sees a header function, so
   `apply_static_renames()` becomes a no-op for headers. Leave both in place: they are still
   needed, and still correct, for the `.cpp`.

4. Record in the code comment why the header case is different from the `.cpp` case, since
   the asymmetry is the whole point: a `.cpp` is split into one set of pieces that share one
   rename map, and a header is not.

## Acceptance Criteria

- The reproduction above splits without falling back, and the program returns 0.
- A regression fixture in `test/`: two headers, one defining a `static inline` function and
  the other calling it, included by one source, with the program checking the value.
- `thread/src/pthread/thread.cpp` no longer falls back.
- The `.cpp` rename still works: `split.static_renames` and `split.static_specifier` keep
  passing, and the mangled name still appears in the split output for a `static` function
  defined in a `.cpp`.

## Outcome

Implemented as option A: an internal-linkage definition in a header or implementation include
is kept in the preamble. `thread/src/pthread/thread.cpp` splits.

Option B -- one rename map for the whole translation unit -- is still the shape the rename was
reaching for, and is still not worth its risk: it changes how split headers are shared between
translation units, and the splitting it would recover is nine small functions in
`platform_time.hpp`.

Fixture: `test/static_in_header_def.hpp` and its user, plus a `static` function in the source
itself so the `.cpp` rename cannot be quietly disabled -- the fixture checks that its mangled
name still appears in the split output.
