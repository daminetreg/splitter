# 30 — A body needs a retracted macro only after another macro expands

**Severity:** Low as a defect, since the fallback keeps the build correct, and Medium as a
hole in a rule this project has now written three times. `TODO/16` and `TODO/25` both fixed a
version of "a definition needs a macro the file `#undef`s". This is the same rule failing for
a reason neither of them covers.

## Motivation

The four-library harness reports **8 fallbacks**, all from two files:

```
libs/assert/test/exp/assert_exp_test.cpp
libs/assert/test/exp/assert_msg_exp_test.cpp
```

all with the same error:

```
assert_exp_test.cpp:54:5: error: use of undeclared identifier 'BOOST_CURRENT_FUNCTION'
#define BOOST_TEST_EQ(expr1,expr2) ( ... __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, ... )
```

They are not new. `assert` was not in the harness's library set when `TODO/16` recorded
62 -> 0, so this is the first run to count them; the binary from before `TODO/28` produces the
same error on the same file.

## Reproduction

```sh
B=example/boost-to-split
CPP_SPLITTER_VERBOSE=1 ./build/cpp-splitter clang++ \
  -I$B/libs/assert/include -I$B/libs/config/include \
  -I$B/libs/core/include -I$B/libs/detail/include \
  -MD -MF /tmp/u.d -MT /tmp/u.o -c -o /tmp/u.o \
  $B/libs/assert/test/exp/assert_exp_test.cpp
```

One fallback, on `BOOST_CURRENT_FUNCTION`.

## Description

`assert_exp_test.cpp` exists to check what `BOOST_ASSERT` expands to under different macro
states, so it toggles them deliberately and re-includes the header between each:

```cpp
#define NDEBUG
#include <boost/assert.hpp>
#undef assert

void test_default_ndebug()            // line 51
{
    std::string v2 = BOOST_STRINGIZE(BOOST_ASSERT(x2));
    BOOST_TEST_EQ( v2, "assert(x2)" );
}
...
#undef BOOST_CURRENT_FUNCTION         // line 67 of the preamble
```

The preamble is the file with the bodies carved out, so it keeps **every** directive,
`#undef BOOST_CURRENT_FUNCTION` included. Every piece includes the preamble, so by the time a
piece compiles its own body the macro is gone -- even for `test_default_ndebug()`, which sits
*above* the `#undef` in the original file and would have compiled fine there.

That is exactly the case `undefined_macros()` was written for. It does not fire here, and the
reason is the finding:

**The body never mentions `BOOST_CURRENT_FUNCTION`.** It mentions `BOOST_TEST_EQ`, which is
defined in `boost/detail/lightweight_test.hpp` and expands to something that uses it. The
check is a text scan of the body against the set of names the file undefines, so it sees one
level and this needs two.

The file also declares locals that *shadow* the macro it retracted:

```cpp
char const * BOOST_CURRENT_FUNCTION = "void test_handler()";
```

so a fix cannot simply keep the macro defined: the source depends on it being gone by that
point. Whatever is done has to preserve the order the file wrote, which is what keeping the
definition in the preamble does.

## Implementation spec

1. **Ask the preprocessor instead of the text.** Parse with
   `CXTranslationUnit_DetailedPreprocessingRecord` and walk the `CXCursor_MacroExpansion`
   cursors whose extent falls inside a definition. That is the set of macros the body actually
   expands, transitively, rather than the set of identifiers someone wrote in it -- and it is
   the same question `undefined_macros()` is already trying to answer.

2. **Intersect that with the file's `#undef` set, as now.** `undefined_macros()` already
   computes the set and `uses_undefined_macro` already carries the answer to both placement
   decisions (`prepare_functions()` and `generate_preamble()`, the second added by `TODO/25`).
   Only the way the body's side of the intersection is computed changes.

3. **Measure the flag before keeping it.** A detailed preprocessing record makes the parse
   more expensive, and this project has twice now shipped a parse option whose cost was not
   measured (`TODO/29`). Time a Boost.Geometry unit with and without it; if it is dear, gate
   it on the file containing at least one `#undef`, which is cheap to test first and false for
   almost every file.

4. **If step 3 says the record is too expensive**, the blunt fallback is defensible for this
   shape: when a file `#undef`s a macro that it does not re-`#define` afterwards, keep every
   definition below the first such `#undef` in the preamble. It splits less, and less is the
   safe direction, but prefer 1-3 -- these two files are pathological and should not set the
   rule for ordinary code.

## Acceptance Criteria

- `assert_exp_test.cpp` and `assert_msg_exp_test.cpp` split with no fallback, and the programs
  they produce still pass.
- A regression fixture: a header defining a macro that expands to a second macro, a source that
  `#undef`s the second one partway through, and a function above the `#undef` whose body
  invokes only the first. It must split, link and run.
- `./test-boost-libraries.sh` reports **0 fallbacks** across the six libraries, and the same
  failed-edge count with and without the splitter.
- Whatever step 3 decides, the parse cost is written down next to the decision.
