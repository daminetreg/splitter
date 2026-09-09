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

**Keep it in place and mark it `inline`, rather than refusing to move it.** An earlier draft of
this file proposed detecting `CXType_IncompleteArray` and leaving the definition in the
preamble untouched. That is worse than it looks: a definition left in the preamble without
`inline` is compiled once per piece, so the array would go from a broken `sizeof` to a
multiple-definition link failure -- `TODO/32` is exactly that failure for a different variable.

C++17 is the assumed default for this project, and `prepare_variables()` already sets
`inline_in_place` for every namespace-scope variable that does not define a type. An array with
a deduced bound keeps its initialiser, so `sizeof` still has a bound to read, and the copies
merge instead of colliding. Nothing needs to detect the array shape at all.

1. **Make C++17 the default.** `environments/monolithic.cmake` sets `CMAKE_CXX_STANDARD 17`,
   and every project built through it follows. The driver's own default is `gnu++14`, so
   leaving this unset means the splitter places against C++14 rules and the move comes back.

2. **Raise anything that pins something older.** `example/spirit-tests/` asked for
   `cxx_std_14` because the Jamfile's `[ requires cxx14_... ]` names it as a minimum; a
   minimum is not a maximum.

3. **The pre-C++17 path stays as it is.** `warn_pre_cxx17_variable_move()` already says once
   per unit that a variable had to be moved because the standard is too old. That warning is
   now the whole of the C++14 story, and it is honest: the move is what an older standard
   forces, and it is why this defect existed.

## Acceptance Criteria

- `spirit_test_lex_regression_wide` splits with no fallback and the program still passes.
- A regression fixture: an array at namespace scope with a deduced bound, and a function that
  takes `sizeof` of it, split into pieces. It must build and print the right count.
- A second fixture proving the refusal is not blanket: an array with a written bound is still
  moved out of the preamble, checked by looking for its `extern` declaration.
- `./benchmark-spirit-tests.sh` loses this fallback.

## Outcome

**Fixed by making C++17 the default**, with no code change to the placement logic.

`inline_in_place` already covered this variable; it was never reached because the benchmark
project pinned `cxx_std_14`. At C++17 the array stays where it is:

```cpp
inline test_data data[] =
{
    { ID_IDENT, L"alpha" },
    ...
```

so `sizeof(data)/sizeof(data[0])` still has a bound to read, and the copies every piece carries
merge rather than colliding. `spirit_test_lex_regression_wide` splits with no fallback and the
program passes.

Worth stating plainly, because the first spec got it backwards: the bug was not that the
splitter moved an array it should have detected and left alone. It was that it was being asked
to place variables under C++14 rules in a project that assumes C++17. Detecting the array shape
would have produced a definition duplicated per piece instead of an incomplete one -- trading
this defect for `TODO/32`'s.

Covered by `launcher.variables_stay_in_place` alongside `TODO/32`, which asserts the array is
still spelled `inline Row rows[]` in the preamble rather than only that the program runs.
