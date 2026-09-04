# 11 — A translation unit whose code arrives through a `.ipp` is not split at all

**Severity:** Medium. Costs a translation unit whenever a project keeps implementations in
an include file with an extension the splitter does not recognise, which is a common Boost
and header-only-library idiom.

## Motivation

`is_header_file()` decides what counts as a header:

```cpp
static const char* exts[] = {".h", ".hpp", ".hxx", ".H", ".h++", ".hh"};
```

`.ipp` is not in that list, and neither are the other conventional implementation-include
extensions (`.inc`, `.inl`, `.tcc`, `.ixx`). Two things follow. The file is never treated as
a splittable header of its own, and -- because `header_split_candidates()` filters on the
same predicate -- it is never even considered as a candidate.

`libs/filesystem/src/utf8_codecvt_facet.cpp` is nothing but a wrapper around one:

```cpp
#define BOOST_UTF8_BEGIN_NAMESPACE  namespace boost { namespace filesystem { namespace detail {
#define BOOST_UTF8_END_NAMESPACE    } } }
#define BOOST_UTF8_DECL             BOOST_FILESYSTEM_DECL

#include <boost/detail/utf8_codecvt_facet.ipp>
```

Every function in that translation unit lives in the `.ipp`. The splitter finds nothing in
the `.cpp` itself, ends up with an empty compilable list, and the launcher passes the whole
command through to the compiler untouched:

```
No function definitions found in .../src/utf8_codecvt_facet.cpp
[cpp-splitter] no compilable files, passthrough: ... -c -o utf8_codecvt_facet.cpp.o ...
```

This is the second of the two translation units on the Boost example that produce no split
objects. Unlike TODO 10 it is not a failure -- the object is correct -- but nothing is
split, so the unit gets none of the benefit.

## Reproduction

Four lines plus an include:

```cpp
// impl.ipp
namespace demo {
int one()   { return 1; }
int two()   { return 2; }
int three() { return 3; }
}
```

```cpp
// b.cpp -- all of this translation unit's code arrives through the .ipp
#include "impl.ipp"
```

```sh
CXX=/usr/local/share/.tipi/clang/4f846ee/bin/clang++
CPP_SPLITTER_VERBOSE=1 CPP_SPLITTER_NO_SERVER=1 \
  ./build/cpp-splitter "$CXX" -std=c++17 -I. -c -o /tmp/b.o b.cpp
```

```
No function definitions found in b.cpp
[cpp-splitter] no compilable files, passthrough: ...
```

`b.o.split` contains no pieces at all. Note the contrast: if `b.cpp` also defines a function
of its own, that one function is split and the three in the `.ipp` are silently left in the
preamble -- so the shortfall is not limited to the all-or-nothing case, it just becomes
invisible.

## Description

The harvest itself is not the problem. Functions defined in a `.ipp` are visited like any
other, because the visitor works from the parent translation unit's AST. What stops them
being split is `header_split_candidates()` rejecting the file, so its functions are never in
the wanted set and their text is copied verbatim into the preamble.

### Implementation plan

1. Add the conventional implementation-include extensions to `is_header_file()`: `.ipp`,
   `.inc`, `.inl`, `.tcc`. Check each against the rest of the splitter first -- the same
   predicate decides the mirrored output layout and the preamble file name, so a `.ipp`
   would start being written to `<split_dir>/include/...` like any other header.
2. Extension lists are a poor way to make this decision. A better rule is available: any
   file that the translation unit `#include`s is a header for these purposes, whatever it is
   called, and `clang_getInclusions()` already provides exactly that set. Prefer deciding
   from the inclusion graph and keep the extension list only for the case where a file is
   passed to the splitter directly on the command line.
3. Where a file is included more than once in a build -- `.ipp` files are sometimes included
   under different macro settings, as the Boost case does with `BOOST_UTF8_DECL` -- confirm
   the per-translation-unit split directories keep those copies apart, and that TODO 04's
   collision guard fires if they ever collide.
4. Note the interaction with TODO 06: an implementation include is by construction not
   parseable on its own, so it must be harvested from its parent translation unit and never
   parsed standalone. That is already how header harvesting works, and it must stay that way
   for these.

## Acceptance Criteria

- The reproduction above produces split pieces for `one`, `two` and `three`, and the
  resulting object still links and runs correctly.
- `libs/filesystem/src/utf8_codecvt_facet.cpp` produces split objects on the Boost example,
  so no translation unit is left unsplit for want of a recognised extension.
- The variant where `b.cpp` defines its own function as well splits both that function and
  the three from the `.ipp`, rather than only the former.
- A program linked against the resulting library still passes the nine filesystem
  assertions used to check the split library end to end.
- Regression fixture in `test/`, registered with `add_test`, covering a `.cpp` whose
  definitions all arrive through an included implementation file.
