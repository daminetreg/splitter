# 33 — Moving an array with a deduced bound leaves an incomplete declaration

**Severity:** Medium, and the same shape as the `constexpr` trap `TODO/26` hit: a variable is
moved whose *type* cannot survive the move, and the failure appears at a use site far away.

## Motivation

The second of the two fallbacks the full Boost.Spirit test suite produces:

```
regression_wide_preamble.h:55:45: error: invalid application of 'sizeof' to an
    incomplete type 'test_data []'
```

`libs/spirit/test/lex/regression_wide.cpp` writes:

```cpp
test_data data[] =
{
    { ID_IDENT, L"alpha" },
    { ID_OPERATION, L"+" },
    ...
};
```

and later, inside a function kept in the preamble:

```cpp
BOOST_TEST(sequence_counter < sizeof(data)/sizeof(data[0]));
```

The variable is moved to the definitions header and this is left behind:

```cpp
extern test_data data[];
```

which is the right declaration for an array of unknown bound and the wrong one for a `sizeof`.

## Description

`TODO/17` moves a namespace-scope variable by writing `extern <type> <name>;` where the
definition was. That is correct whenever the type can be written in front of a name, which
`variable_declaration_head()` already checks -- but an array whose bound comes from its
initialiser has no bound to write. `test_data data[]` is a complete type only where the
initialiser is; anywhere else it is incomplete, and `sizeof` on it is ill-formed.

The parallel with `TODO/26` is exact and worth stating: there, a `BOOST_CONSTEXPR_OR_CONST`
variable was moved because the *text* said nothing about const-ness, and the fix was to ask the
type rather than the spelling. Here the text says nothing about the bound either, and the
answer is again in the type.

## Reproduction

```sh
cmake -GNinja -S example/spirit-tests -B /tmp/sp \
    -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake \
    -DBOOST_ROOT_DIR=$PWD/example/boost-to-split \
    -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
CPP_SPLITTER_VERBOSE=1 ninja -C /tmp/sp -j8 spirit_test_lex_regression_wide
```

## Implementation spec

1. **Refuse to move an array of unknown bound.** In `prepare_variables()`, ask libclang for the
   variable's type: `clang_getArrayElementType()` on a `CXType_IncompleteArray` identifies
   exactly this case. Do not test the spelling for `[]`; a typedef can hide it, and asking the
   type is the lesson `TODO/26` already paid for.

2. **A bound that is written keeps working.** `test_data data[7]` is a complete type in any
   declaration, so `CXType_ConstantArray` stays movable. The refusal is specifically for
   `CXType_IncompleteArray`.

3. **Say so where the other bail-outs are.** `variable_declaration_head()` already lists the
   cases that keep a variable where it is; this belongs with them, with the reason, not as a
   separate check somewhere else.

## Acceptance Criteria

- `spirit_test_lex_regression_wide` splits with no fallback and the program still passes.
- A regression fixture: an array at namespace scope with a deduced bound, and a function that
  takes `sizeof` of it, split into pieces. It must build and print the right count.
- A second fixture proving the refusal is not blanket: an array with a written bound is still
  moved out of the preamble, checked by looking for its `extern` declaration.
- `./benchmark-spirit-tests.sh` loses this fallback.
