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

## Outcome

Both defects are fixed, and both turned out to have a different cause than the analysis
above gave them. What follows records what they actually were; the sections above are left
as written so the difference between a plausible mechanism and a verified one stays visible.

### Defect 1 — the split decision was never the problem

The functions in `struct transform` were already kept in the header, and always had been: a
macro invocation contains no `{`, so `definition_decl_end()` finds no body to move and the
keep rule fires. Nothing was ever split out of that class.

What destroyed it was the always-inline strip. A definition produced by a macro has the
invocation as its extent -- and so does every attribute on it, for the same reason: that is
where the tokens entered the file. The attribute's range and the definition's range are
therefore the same range, and `strip_always_inline()` erased it, then prepended the `inline `
it adds whenever the text it returns no longer says `inline`. That single `inline` is the
whole of what was left.

The fix is one condition: an attribute range that spans the entire extent is left alone,
because it cannot have come from the text it would be cut out of. Definitions that share one
extent are now also kept in the header by an explicit rule rather than by the accident of
having no brace, since `generate_preamble()` collapses such extents and splitting one of
them would silently take its siblings with it.

Fixture: `test/macro_members_header.hpp`.

### Defect 2 — the reachability analysis was right; the conditional replay was wrong

`collect_emitted()` had nothing to do with these link failures. Every one of them was a
definition the splitter *did* split out, into a piece that compiled to an **empty object**.

A definition written inside `#if C` is written into its split file inside the same `#if C`,
so that a body the real build never sees is not compiled. But the piece replays that
condition *after* including the whole header, and that is a different point in the
preprocessor's life. Two headers here make the replayed condition false:

* `boost/core/demangle.hpp` defines `BOOST_CORE_HAS_CXXABI_H`, writes `demangle`,
  `demangle_alloc` and `demangle_free` under `#if defined(BOOST_CORE_HAS_CXXABI_H)`, and
  `#undef`s it on its last line. This is the same trap `undefined_macros()` already guarded
  the *body* against; the conditionals were simply not being checked too. 39 of the 40
  failures were this one header.

* `boost/spirit/home/support/char_encoding/ascii.hpp` spells its include guard
  `#if !defined(BOOST_SPIRIT_ASCII_APRIL_26_2006_1106PM)` rather than `#ifndef`.
  `active_conditionals()` recognised only the `#ifndef` spelling, so the guard was replayed
  as an ordinary condition -- and a guard replayed after its own header has been included is
  always false. This is what made the Spirit consumer fall back.

Neither failure is visible when it happens. The piece compiles, the object is written, the
link merges it, and the only symptom is an undefined reference to a function whose
definition is plainly there in the source.

Fixture: `test/retracted_macro_header.hpp`, which carries both spellings.

### The correction

The claim above that "**that reasoning was wrong**" -- that the reachability roots were kept
too narrow, and that a missed root can leave a function emitted by no object at all -- does
not survive being checked. It was inferred from the shape of the error, `undefined reference
to an inline function in a header`, without looking at the object that was supposed to
define it. That object existed and was empty, for a reason that has nothing to do with
reachability. The original narrow-roots argument in `collect_emitted()` stands unchanged.

### Two more causes behind the same symptom

Clearing the first two left one failing link, and it turned out to be two more defects with
nothing in common but their symptom -- which is the point worth keeping: `undefined
reference to <a function that is plainly defined>` is not a diagnosis, it is a category.

* **A constructor defined in an implementation include.** A constructor is a family of
  symbols -- C1 complete-object, C2 base-object -- and splitting one writes it `inline` with
  `__attribute__((used))`, which forces exactly one of them into existence: the piece emits
  C2 while every caller asks for C1. Constructors in headers are kept in the preamble for
  that reason, but `.ipp` did not count as a header, and Boost writes the whole of
  `utf8_codecvt_facet` in one. `is_included_file()` now covers `.ipp`, `.inl`, `.inc` and
  `.tcc`.

* **A definitions header no piece included.** Definitions that may exist in only one object
  go to a second header, which exactly one split piece includes -- the first compilable one.
  `boost/archive/../xml_grammar.cpp` is a translation unit whose only definition is an
  explicit specialization, so every definition was kept and there was no compilable piece to
  carry it. The definitions header is now given a piece of its own when that happens.

  Making it compile then needed one more thing: an explicit specialization is not declared by
  its class body -- what is written there is the primary template's member -- so moving the
  definition into the definitions header put it after every use that instantiates it, and the
  compiler rejected it outright. A declaration now stays behind in its place.

Fixtures: `test/ipp_ctor_header.hpp`, and `launcher.explicit_specialization`, which needs two
translation units because the specialization has to be *needed* from another object rather
than inlined.

### Where it stands

| | plain | split | was |
|---|---:|---:|---:|
| objects built | 457 | 457 | 457 |
| **failed edges** | **0** | **0** | 40 |
| translation units split | — | 373 | 373 |
| fallbacks to plain compilation | — | 62 | 62 |

| Spirit consumer | time | result |
|---|---:|---|
| plain | 4.1s | compiles |
| split | 32.2s | 4525 pieces, **no fallback**, links and runs with identical output |

The filesystem example still splits 12 of 12 with no fallbacks and scores 9/9; the test
suite is 15 tests, four of them new.

The 62 fallbacks are unchanged and unexplained -- they are not link failures, so this run
does not say what they are. That is the next thing to look at.
