# 24 — What adding Boost.Geometry's test suite found

**Severity:** High while it lasted. Four defects, none of them visible in the four libraries
the harness covered before, and one measurement trap that manufactured two more that did not
exist.

**Status: four fixed. The fifth was misdiagnosed as structural; it is TODO 25.**

## Motivation

`./test-boost-libraries.sh` covered `filesystem;spirit;system;core`. Those are three shapes of
code: ordinary library sources, small test translation units, and Spirit's expression
templates. Boost.Geometry is a fourth. It is header-only like Spirit, but where Spirit's
weight is in expression templates, Geometry's is in tag dispatch over a large concept
hierarchy — and unlike Spirit it has a real test suite: 24 targets that are programs rather
than compile checks, built on Boost.Test in header-only mode.

That last detail is what made it productive. Including Boost.Test as headers drags
`unit_test_main.ipp`, `unit_test_log.ipp` and `decorator.ipp` into every one of those 24
translation units, and those files use idioms nothing else in the corpus does.

Adding it takes the corpus from 457 objects to 583.

## What it found

Everything **compiled and linked correctly from the first run**: 583 objects either way, no
target failing that would not also fail without the splitter. What geometry broke was
*splitting*: 125 of the 583 units fell back to compiling whole, and essentially every one of
the 24 test targets was among them.

Four distinct causes, each found only after the one in front of it was cleared. They are
listed in the order they surfaced, which is also the order of how much they hid:

### 1. A macro-produced definition moved as a fragment of its invocation

Boost.Test writes

```cpp
BOOST_TEST_SINGLETON_CONS_IMPL(collector_t)
```

which expands to one member function. The extent libclang reports for that function is the
two characters `t)` — the tail of the argument and the closing paren. That fragment has
external linkage and is not inline, so it went to the definitions header, taking `t)` with it
and leaving the invocation open:

```
decorator.ipp:143:2: error: embedding a #include directive within macro arguments
                            is not supported
```

21 of the first 30 units classified.

### 2. GCC's system headers handed to clang's libclang

`detect_system_includes()` ran `g++` regardless of which compiler the build uses. GCC's
`xmmintrin.h` implements the SSE intrinsics with `__builtin_ia32_*`, which clang does not
have, so every parse that reached it failed:

```
xmmintrin.h:136: error: use of undeclared identifier '__builtin_ia32_addss'
```

1790 of the errors in that build were this one header. Nothing in Boost reached
`<xmmintrin.h>` until Geometry, which pulls it in through Boost.Multiprecision.

### 3. `main` written into a split piece as `inline`

A split piece taken out of a header is written `inline`, so several objects may carry the
same definition and the linker merge them. `main` may not be inline, and Boost.Test defines
it in `unit_test_main.ipp`:

```
unit_test_main.ipp:292:1: error: 'main' is not allowed to be declared inline
```

### 4. A signature mentioning a type with no linkage

```cpp
namespace { struct unit_test_log_data_helper_impl { ... }; }
bool log_entry_start( unit_test_log_data_helper_impl& current_logger_data ) { ... }
```

A type in an unnamed namespace has no linkage of its own, so a function taking it can only be
defined in the translation unit that declares the type:

```
unit_test_log.ipp:263: error: function 'boost::unit_test::log_entry_start' is used but not
defined in this translation unit, and cannot be defined in any other translation unit
because its type does not have linkage
```

The function's *own* linkage says nothing here — libclang reports `log_entry_start` as
external, because it is the type that is unique to the unit. After the first three fixes this
was what every remaining geometry test fell back on.

## The measurement trap

Worth more than any single defect above, because it produced two findings that were not real.

At `-j32` this corpus exhausts 122 GiB. Each Geometry test translation unit is a large
template instantiation on its own, and the splitter holds a libclang AST of the same unit
alongside the compile. The OOM killer took 52 processes, and ninja reported **35 failed
objects** — which looks exactly like a splitter defect that has stopped falling back safely
and started emitting broken code. It is not one.

Worse, one of the killed processes left a half-written *shared* rewritten header behind.
Headers are split once and reused by every unit that includes them, so the next unit to come
along inherited a truncated `boost/variant2/variant.hpp` and failed with

```
variant.hpp:1439:82: error: no template named 'variant_cc_base'
```

which is a perfectly convincing report of a splitting bug in a file the splitter had, on that
run, never finished writing.

Two rules follow, and both are now in the code rather than in someone's memory:

* The job count is a knob — `CPP_SPLITTER_TEST_JOBS`, default 32 — with the reason written
  beside it. Geometry wants 8.
* A build that reports OOM kills is not a measurement. `grep -c '^Killed'` on the log before
  reading anything else out of it.

## Cost

Splitting Geometry is expensive, and the expense is the point of the exercise rather than a
surprise: at `-j8` the plain build of the 583 objects takes 140s, and before these fixes the
split build took 3372s — with 125 units *not* splitting, which is the cheap path. With them
splitting, it is considerably slower still. A Geometry test translation unit produces
thousands of pieces, and each piece re-instantiates the part of the concept hierarchy its one
function needs.

That is worth stating plainly: this corpus is now a correctness harness, not something to run
in a loop. `benchmark-spirit-split.sh` and `benchmark-boost-split.sh` are where timing
questions belong.

## Reproduction

```sh
CPP_SPLITTER_TEST_JOBS=8 ./test-boost-libraries.sh 'filesystem;spirit;system;core;geometry'
./classify-fallbacks.sh     # groups whatever fell back by its first error
```

## Acceptance Criteria

- No target fails that would not also fail without the splitter. **Met from the first run**,
  and unchanged by every fix since.
- The four causes above have regression fixtures in `test/` that fail without their fix.
- `grep -c '^Killed'` on the split log is zero, or the run is discarded.
- The four libraries the harness covered before keep splitting with no fallbacks.

## Outcome

All four causes are fixed, each with a fixture that fails without its fix:

| | fixture |
|---|---|
| macro-produced definition moved as a fragment | `test/macro_definition_header.hpp` |
| GCC's system headers given to clang | covered by `test/no_linkage_type_*` reaching `<vector>` |
| `main` written into a piece as `inline` | `test/main_in_header_impl.ipp` |
| signature mentioning a no-linkage type | `test/no_linkage_type_impl.ipp` |

Two of those came with a second finding attached. `--cxx` was still asking `g++` for system
includes even when told which compiler to use, and the command-line driver parsed at the
compiler's default while compiling its pieces at `-std=c++17` -- TODO/19's bug mirrored, and
found the same way: by a fixture that would not build.

### The performance one

The no-linkage check walks canonical types and recurses into their template arguments. Doing
that for every harvested function is affordable on ordinary code and is not on Geometry, where
the canonical form of one expression type is enormous: `libs/geometry/test/util/range.cpp`
went from about a minute to **over half an hour**, pinned at 99.9% CPU in the harvest.

Almost every function is kept for a cheaper reason first, so the question only has to be asked
of the few that would otherwise be split. Asking it in `prepare_functions()` after the cheap
rules, rather than in the visitor, brought that unit back to **51 seconds** with the same
decisions. This is worth remembering as a shape: a check that is correct and cheap on the
corpus you have can be quadratic on the corpus you add.

### What is left — and the claim this file made about it was wrong

With all four fixed, `range.cpp` splits into 6347 pieces and then falls back on this:

```
progress_monitor.ipp_definitions.h:19:5: error: use of undeclared identifier 'PM_SCOPED_COLOR'
```

Boost.Test's `progress_monitor.ipp` defines `PM_SCOPED_COLOR` at line 117, uses it in four
member functions, and `#undef`s it at line 182. Those functions have external linkage and are
defined out of line, so they may exist in only one object -- which means the definitions
header -- and the definitions header includes the preamble first, by which point the macro is
gone.

**An earlier revision of this file concluded from that "there is no placement that works" and
called the file unsplittable under the current design. That was wrong, and it was wrong in a
way worth keeping visible.** The reasoning was sound in isolation and was never tested: one
cold build failed, and the conclusion was written from it. Building the same target twice
disproves it in about a minute --

| run | fallbacks | units split |
|---|---:|---:|
| cold | **1** (`area.cpp` itself) | 50 |
| warm, after `touch area.cpp` | **0** | 51 |

-- the same unit that "cannot be split" splits on the second run. What that exposed is not one
structural limit but two ordinary defects, now filed as **TODO 25**: a definition that needs a
macro the file undefines is moved into the definitions header anyway, and a header split on a
previous run contributes none of its pieces to the object. The second is why the warm build
"succeeds": it silently drops them.

The general shape is still worth recording, because it is what made the wrong conclusion
plausible: **a strong out-of-line definition that depends on transient macro state, in a
header that every piece includes**. It has a placement -- see TODO 25 -- it just was not the
one being looked for.

### Geometry is not in the default library set

It is supported, and asking for it by name is one command:

```sh
CPP_SPLITTER_TEST_JOBS=8 ./test-boost-libraries.sh 'filesystem;spirit;system;core;geometry'
```

But a Geometry test translation unit takes about a minute to split against five seconds to
compile, and there are 24 of them plus their dependencies -- hours per run. That is a finding,
not a cost worth paying on every invocation. The default set stays
`filesystem;spirit;system;core;smart_ptr;assert`, which still splits with no fallbacks at all.
