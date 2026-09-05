# 16 — Failures found by splitting more of Boost than the filesystem example

**Severity:** High. Two distinct defects, both invisible to the twelve-translation-unit
example everything has been measured against until now.

## Motivation

`./test-boost-libraries.sh` builds a wider slice of Boost, including its test suites, twice:
once through the splitter and once without. Only the difference is counted, so a test that
does not build normally is not held against the splitter.

With `filesystem;spirit;system;core`:

| | plain | split |
|---|---:|---:|
| objects built | 457 | 457 |
| **failed edges** | **0** | **40** |
| translation units split | — | 373 |
| fallbacks to plain compilation | — | 62 |

Everything compiles. Forty *links* fail, and all forty are test executables that link
cleanly without the splitter.

Spirit needs separate treatment, being header-only with no CMake test suite. Compiled
directly, it is the densest template translation unit available here:

| | time | result |
|---|---:|---|
| plain | 4.0s | compiles |
| split | 30.8s | 4525 pieces, then **falls back** |

## Defect 1 — a member from a macro expansion cannot be split

Spirit's fallback traces to Boost.Proto. `libs/proto/include/boost/proto/transform/impl.hpp`
contains:

```cpp
    struct transform
    {
        BOOST_PROTO_TRANSFORM_(PrimitiveTransform, X)
    };
```

That macro expands to several members, including functions. The splitter harvests one of
them, removes its extent -- which is the macro invocation, since that is where the tokens
come from -- and writes the rewritten header as:

```cpp
    struct transform
    {

inline

    };
```

The member is gone, and a bare `inline` is left in its place inside a class body, which is a
syntax error. Everything downstream of that header then fails, and the translation unit
falls back.

`generate_preamble()` already knows extents can be shared, and collapses identical ones so a
macro is not emitted once per function it expands to. What it does not do is decline to
*split* them. Neither the definition nor the declaration of such a function can be
reconstructed from its source text, because that text is a macro invocation covering several
declarations at once.

**The rule:** a function whose extent is shared with another function, or whose extent text
does not parse as a declarator, must be kept in the header. The information is already
available where extents are collapsed; it needs to reach `should_keep_in_header()`.

## Defect 2 — inline functions the splitter stops emitting are needed by other objects

The forty failing links are all this shape:

```
lightweight_test_test2.cpp.o: in function `boost::core::detail::fix_typeid_name(char const*)':
type_name.hpp:67: undefined reference to `boost::core::demangle(char const*)'
```

`demangle` is an inline function in a header. `collect_emitted()` decides per translation
unit which functions that unit emits, and its answer here is correct for the unit in
isolation: nothing the unit is obliged to emit reaches `demangle`.

But splitting has already changed the question. A function that would have been inlined into
its caller is now called out of line, because the caller's body was moved into a piece that
sees only a declaration. So the unit *does* need `demangle` as a symbol, and the reachability
analysis -- which models what the compiler would have emitted from the *unsplit* source --
says it does not.

This is the same tension recorded in `TODO/14`, seen from the other side. There, being too
eager about what to emit produced dangling references to functions Boost deliberately leaves
undefined. Here, being too conservative produces dangling references to functions that are
defined but that nobody emits.

The reachability roots were deliberately kept narrow on the grounds that missing a root only
costs some splitting and breaks nothing. **That reasoning was wrong**, and this is the
counterexample: a missed root can mean a function is emitted by no object at all.

**The rule:** a function that any split piece of this unit *calls* is a root, whether or not
the unsplit source would have emitted it. The call graph `collect_emitted()` already builds
has the edges; what is missing is that the pieces themselves are entry points, not just the
definitions the compiler must emit.

## Reproduction

```sh
./test-boost-libraries.sh 'filesystem;spirit;system;core'
```

The script reports both, and names the targets that fail only under the splitter. For defect
1 alone, the Spirit consumer at the end of its output is enough.

## Acceptance Criteria

- `./test-boost-libraries.sh` reports the same number of failed edges with and without the
  splitter, for the default library set.
- `example/spirit_example.cpp` splits without falling back, and the object it produces links
  and runs.
- No generated header contains a class member replaced by a bare specifier.
- The filesystem example still splits 12 of 12 with no fallbacks and passes its nine
  assertions, and the eleven tests still pass.
- Two regression fixtures in `test/`: a class whose members come from a macro expansion, and
  a translation unit whose split pieces call an inline function the unsplit source would have
  inlined away.
