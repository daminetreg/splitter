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

1. **Harvest variables from the headers too, not only from the unit's source.** `harvest_variable()`
   already records what it is given; the `wanted` set is what limits it to the unit. A header
   that is a split candidate is already in that set, so its variables are reachable -- what is
   missing is passing them to the placement decision for the *unit's* preamble.

2. **Move a header's namespace-scope variable the same way a source's is moved.** The
   treatment exists: definition into the definitions header, which exactly one piece compiles,
   and an `extern` declaration left where it was. The declaration has to be emitted into the
   rewritten header copy, not into the unit preamble, because that is the file every piece
   reads.

3. **Do not mangle these.** `TODO/26` mangles internal-linkage variables because two units may
   both define one. A header's *external*-linkage variable is a different case: renaming it
   would break any other translation unit that refers to it by its real name. Move it and leave
   the name alone.

4. **Keep the existing bail-outs**, and note that `TODO/33` adds one: an array whose bound
   comes from its initialiser cannot be declared `extern` without losing its size.

## Acceptance Criteria

- `spirit_test_qi_uint_radix` splits with no fallback and the program still passes.
- A regression fixture: a header defining a `char const*` at namespace scope, included by a
  source that splits into more than one piece, where each piece touches the variable. The
  program must link and print the value once.
- `nm` on the final object shows exactly one definition of that symbol, because it links either
  way once the duplicates are gone.
- `./benchmark-spirit-tests.sh` loses this fallback and the whole four-library harness is
  unchanged.
