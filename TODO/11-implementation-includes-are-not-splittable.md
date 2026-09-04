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

The extension list is the wrong instrument, and adding `.ipp` to it would be treating a
symptom. `clang_getInclusions()` already reports exactly which files the translation unit
includes, which is what "header" means here; the extension test is a guess at the same
question, made without the information. So the direction is to remove it from the internal
decisions rather than extend it.

Measured, not assumed: deleting the `is_header_file(inc_path)` filter from
`header_split_candidates()` is **not** on its own enough. The `.ipp` does become a
candidate, and is then rejected by the include-guard rule from TODO 06:

```
Skipping impl.ipp: no include guard, so it is one half of a pair and is not meant to be
included on its own
```

which is the crux of this item. An implementation include legitimately has no include
guard, for the same reason a header/footer pair member has none, and TODO 06 cannot tell
them apart. That heuristic has to be sharpened before the extension check can go.

1. Sharpen the include-guard rule so it distinguishes the two cases it currently conflates.
   A file with no guard that is included **once** in the translation unit is an
   implementation include and is safe to split; one included **repeatedly** is half of a
   pair and is not. `clang_getInclusions()` reports every inclusion, including repeats, so
   the count is available without any new parsing.
2. Then remove the `is_header_file(inc_path)` filter from `header_split_candidates()`. The
   list it filters comes from `clang_getInclusions()` already, so the test is pure
   redundancy on top of better information.
3. Exclude the main source file from the candidate list explicitly. `clang_getInclusions()`
   reports it as an inclusion of itself, so once the extension filter is gone the
   translation unit starts nominating its own `.cpp` as a header to split -- visible in the
   same experiment above, which reported `Skipping b.cpp: no include guard`.
4. Replace the remaining internal uses of `is_header_file()` with the information their
   callers already have, rather than re-deriving it from the file name:
   - `header_split_candidates()` and `resolve_header_deps()` know a path came from the
     inclusion set, so anything they pass on is a header by construction;
   - `split_unit()` takes `input_is_header` as a parameter instead of recomputing it, since
     both call sites know which case they are in;
   - the `is_ctor_or_dtor && is_header_file(fn.file)` rule in `prepare_functions()` becomes
     "the defining file is not the main file", which the harvest already records;
   - the scan at the end of CLI mode that looks for a header preamble by file name should
     read the path out of the `.split` manifest, which records it exactly.
5. Keep a syntactic test for one place only: `is_splittable_file()`, which decides from
   `argv[1]` whether the tool was invoked on a source file or as a compiler launcher. That
   runs before anything is parsed, so no inclusion information exists yet. Rename it to make
   the narrow purpose obvious, and let `build_clang_flags()`'s `-x c++-header` decision for a
   directly-named file follow from the same test.

## Acceptance Criteria

- The reproduction above produces split pieces for `one`, `two` and `three`, and the
  resulting object still links and runs correctly.
- A header/footer pair, included repeatedly and carrying no include guard, is still skipped:
  the sharpened rule must not readmit what TODO 06 was written to exclude.
- The translation unit's own source file never appears in its list of headers to split.
- `libs/filesystem/src/utf8_codecvt_facet.cpp` produces split objects on the Boost example,
  so no translation unit is left unsplit for want of a recognised extension.
- The variant where `b.cpp` defines its own function as well splits both that function and
  the three from the `.ipp`, rather than only the former.
- A program linked against the resulting library still passes the nine filesystem
  assertions used to check the split library end to end.
- Regression fixture in `test/`, registered with `add_test`, covering a `.cpp` whose
  definitions all arrive through an included implementation file.
