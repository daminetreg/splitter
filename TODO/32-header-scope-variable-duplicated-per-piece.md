# 32 — A namespace-scope variable defined in a header is copied into every piece

**Severity:** Medium. The link fails and the unit falls back, so nothing silently wrong ships —
but a fallback writes no split cache, so that unit then repeats the whole split and compiles
plain on every build for ever (`TODO/31` measures what that costs).

## Motivation

Found by `./benchmark-spirit-tests.sh` once it covered the whole suite:

```
ld: .../utf8.hpp_1_utf8_put_encode.o: multiple definition of `unsigned_overflow_base35';
    .../uint_radix.cpp_0_definitions.o: first defined here
```

`libs/spirit/test/qi/uint_radix.hpp` defines, at namespace scope in a **header**:

```cpp
char const* max_unsigned_base35 =               "2BR45QA";
char const* unsigned_overflow_base35 =          "2BR45QB";
char const* digit_overflow_base35 =             "2BR45QA0";
```

Note what is const: the *pointee*, not the pointer. `char const*` is an ordinary object with
external linkage, so each of these is a definition, not a declaration.

## Description

`TODO/17` and `TODO/26` moved namespace-scope variables out of the preamble because the
preamble is included by every piece, so a definition left in it is one object per piece rather
than one per translation unit. Both of them look at the variables **of the unit's own source**.
`generate_preamble()` is handed the `.cpp`'s variables and moves those.

A variable defined in an *included header* is not in that list. It arrives because the preamble
`#include`s the header in the ordinary way, and every piece includes the preamble -- so the
definition is compiled once per piece and `ld -r` rejects the copies. The error above names two
of them: a piece split out of an unrelated header, and the definitions object.

This is not what `TODO/23` declined. That was about a definition **split out of** a header,
where two translation units splitting the same header separately would have to agree on a
mangled name. Here nothing is split out of `uint_radix.hpp` at all; the definition is simply
duplicated inside one program, and the fix is confined to one unit's own split directory.

### Why the original is legal and this is not

Included by exactly one translation unit, as the Jamfile builds it, one definition exists and
the program is fine. Two translation units including it would be an ODR violation in plain
Boost too -- but that is not what happens here. The splitter turns one translation unit into
several objects, and it is the splitter that creates the second definition.

## Reproduction

```sh
cmake -GNinja -S example/spirit-tests -B /tmp/sp \
    -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake \
    -DBOOST_ROOT_DIR=$PWD/example/boost-to-split \
    -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
CPP_SPLITTER_VERBOSE=1 ninja -C /tmp/sp -j8 spirit_test_qi_uint_radix
```

## Implementation spec

**Keep the definition where it is and mark it `inline`. Do not move it.** C++17 is the assumed
default for this project (`environments/monolithic.cmake`), an `inline` variable has vague
linkage, and the copies every piece gets then merge -- which is precisely what the preamble
needs from anything it carries. Moving is the treatment that has to justify itself, not the
other way round: `TODO/33` is a variable that cannot survive a move at all, and `TODO/26` is a
constant that stopped being a constant expression when it was moved.

`prepare_variables()` already does this. It sets `inline_in_place` whenever the standard is
C++17 or later and the declaration does not define a type, and `generate_preamble()` emits the
`inline`. The machinery was simply never reached for this header.

1. **Split a header that defines variables and no functions.** `resolve_header_deps()` reads
   both `fns` and `vars` out of the harvest and then skips on `fns.empty()` alone, discarding
   the variables and writing a "no function definitions" manifest. A header skipped that way is
   never rewritten, so the preamble includes the original and every piece compiles its
   definitions again. The condition is `fns.empty() && vars.empty()`, which is already what
   `split_unit()` uses for the same decision one level down.

2. **Nothing else changes.** Once the header is rewritten, `prepare_variables()` marks its
   variables `inline` in the mirrored copy, which sits ahead of the original on the include
   path, and the copies merge.

3. **Do not mangle the name.** `TODO/26` mangles internal-linkage variables because two units
   may each define one. An `inline` variable is one entity by definition; renaming it would
   break any other translation unit that refers to it.

## Acceptance Criteria

- `spirit_test_qi_uint_radix` splits with no fallback and the program still passes.
- A regression fixture: a header defining a `char const*` at namespace scope, included by a
  source that splits into more than one piece, where each piece touches the variable. The
  program must link and print the value once.
- `nm` on the final object shows exactly one definition of that symbol, because it links either
  way once the duplicates are gone.
- `./benchmark-spirit-tests.sh` loses this fallback and the whole four-library harness is
  unchanged.

## Outcome

**Fixed, by splitting the header rather than by moving anything.**

`resolve_header_deps()` now skips only when a header has neither functions nor variables. With
the header rewritten, the existing C++17 path marks its definitions `inline` in the mirrored
copy:

```cpp
inline char const* max_unsigned_base3 =                "102002022201221111210";
```

`spirit_test_qi_uint_radix` splits with no fallback and the program passes.

The fix is one condition. The reason it took a file to find is that the symptom -- a multiple
definition reported against a piece of an unrelated header -- points nowhere near
`resolve_header_deps()`, and the variables were being read out of the harvest correctly and
then dropped on the next line.

Covered by `launcher.variables_stay_in_place`, which asserts the mirrored copy says `inline`
rather than only that the program runs. Verified it fails without the fix, with
`cpp-splitter fell back instead of splitting`.
