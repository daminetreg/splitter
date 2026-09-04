# 11 — A translation unit whose code arrives through a `.ipp` is not split at all

**Severity:** Medium. Costs a translation unit whenever a project keeps its implementations
in an include file rather than in the `.cpp`, which is a common Boost and
header-only-library idiom.

Two independent rules reject such a file, and they have to be fixed together: the extension
list in `is_header_file()`, and the include-guard heuristic added for TODO 06. Removing the
first on its own changes nothing, which was measured rather than assumed -- see the
implementation plan.

**Status: implemented.** The `.ipp` is now recognised, split, and its pieces compiled.
`libs/filesystem/src/utf8_codecvt_facet.cpp` no longer passes through untouched; it now
fails on **TODO 10** instead, which both remaining translation units are blocked on. See
"Outcome" at the end.

## Motivation

An implementation include is a file that carries definitions and is included from exactly
one place, so it is written without an include guard and given an extension that marks it as
not-a-header: `.ipp`, `.inc`, `.inl`, `.tcc`. Both of those properties currently disqualify
it from being split, for different reasons.

The first is `is_header_file()`, which decides what counts as a header by extension:

```cpp
static const char* exts[] = {".h", ".hpp", ".hxx", ".H", ".h++", ".hh"};
```

`.ipp` is not in that list, so `header_split_candidates()` -- which filters on the same
predicate -- never considers the file.

The second is the include-guard heuristic from TODO 06: a file with no include guard is
taken to be one half of a header/footer pair and skipped, because rewriting half a pair is
never correct. An implementation include has no guard either, and the rule cannot tell the
two apart. This gate sits behind the first, so it only becomes visible once the extension
filter is removed.

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

`b.o.split` contains no pieces at all. Delete the `is_header_file(inc_path)` filter from
`header_split_candidates()` and rebuild, and the same command reports the second gate
instead:

```
Skipping impl.ipp: no include guard, so it is one half of a pair and is not meant to be
included on its own
Skipping b.cpp: no include guard, ...
```

still with no pieces produced -- and with the translation unit now nominating its own source
file, because `clang_getInclusions()` lists the main file among its inclusions. Note the contrast: if `b.cpp` also defines a function
of its own, that one function is split and the three in the `.ipp` are silently left in the
preamble -- so the shortfall is not limited to the all-or-nothing case, it just becomes
invisible.

## Description

The harvest itself is not the problem. Functions defined in a `.ipp` are visited like any
other, because the visitor works from the parent translation unit's AST and does not care
what the file is called. What stops them being split is `header_split_candidates()`
rejecting the file, so its functions never enter the wanted set and their text is copied
verbatim into the preamble instead.

That rejection happens twice over, once per rule above, which is why the fix is not a
one-line change to an extension list.

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
  taking it from 10 of 12 translation units splitting to 11 (12 once TODO 10 is fixed too).
- The variant where `b.cpp` defines its own function as well splits both that function and
  the three from the `.ipp`, rather than only the former.
- A program linked against the resulting library still passes the nine filesystem
  assertions used to check the split library end to end.
- Regression fixture in `test/`, registered with `add_test`, covering a `.cpp` whose
  definitions all arrive through an included implementation file.

## Outcome

Implemented, and it took four changes rather than the two the plan expected -- the extra two
only became visible once the first gates were open.

- `header_split_candidates()` no longer filters by extension. Its input already came from
  `clang_getInclusions()`, which is what "header" means here.
- The include-guard rule is sharpened as planned: a file with no guard read **more than
  once** is one half of a pair and skipped; read exactly once it is an implementation
  include and is split. `clang_getInclusions()` reports repeats, so the count needs no extra
  parsing. The four pair-halves on the Boost example -- `detail/header.hpp`,
  `detail/footer.hpp`, `abi_suffix.hpp`, `iterator/detail/config_undef.hpp` -- are all still
  skipped.
- The file being compiled is excluded from its own candidate list, since
  `clang_getInclusions()` reports it among its own inclusions.
- `split_unit()` and `prepare_functions()` take `input_is_header` from the caller rather than
  guessing from the file name. This is required, not cleanup: an implementation include is a
  header here whatever it is called, and deciding otherwise sends its output to the wrong
  directory and strips the `inline` its pieces need.

Two further defects surfaced only once the file was actually reaching the splitter:

- **The preamble was never written for a unit with no functions of its own.** Pieces split
  out of a header include the translation unit's preamble for context (TODO 09), and a unit
  whose whole body arrives through an include has nothing of its own, so the file the pieces
  included did not exist: `fatal error: 'utf8_codecvt_facet_preamble.h' file not found`.
  The preamble is now written whenever the unit is the one being compiled, whether or not it
  has functions of its own.
- **The reachability roots were restricted to the file being compiled.** With every
  definition living in the `.ipp`, that left no roots at all, so nothing was reachable and
  every function was kept in the header -- the file was split into zero compilable pieces.
  A non-inline definition with external linkage is emitted by this translation unit whatever
  file it was written in, so the restriction is gone.
- The launcher also treated an empty `compilable_files` as nothing to do. When a unit's code
  is all in an include, the pieces are the header's and live in `header_obj_files`, so both
  are now checked.

Verified:

| check | before | after |
|---|---|---|
| `.ipp` recognised and split (Boost) | no | **yes** |
| translation units passed through untouched | 1 | **0** |
| pair-halves still skipped | 4 | **4** |
| program linked against the split library | 9/9, 0 undefined | **9/9, 0 undefined** |
| tests | 8 | **9** |

`test/impl_include.ipp` with `test/impl_include_main.cpp` is the regression fixture: a
translation unit with no definitions of its own, all of them arriving through an
implementation include. It splits into three pieces, compiles, links and runs with no
fallback.

### Still blocked

`utf8_codecvt_facet.cpp` does not yet produce split objects, so the Boost example stays at
10 of 12. It now gets as far as `ld -r` and fails there on **TODO 10**: `do_in`, `do_out`
and `do_length` are virtual overrides kept in the preamble, non-inline with external
linkage, so every piece emits them. That is the same cause as `exception.cpp`, which means
both remaining translation units are now blocked on TODO 10 alone.
