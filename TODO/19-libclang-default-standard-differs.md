# 19 — libclang parses at a different `-std` than the compiler compiles at

**Severity:** High, and higher than its 7 fallbacks suggest. The splitter decides what to
split by parsing one program and then compiles a different one. Here it fails loudly; there
is no reason it always would.

7 of the 62 fallbacks in the wider Boost run.

## Motivation

When the compile command carries no `-std`, the driver and libclang each fall back to their
own default, and with the toolchain here those defaults differ:

```
$ clang++ --version
clang version 13.0.0
$ echo | clang++ -x c++ -dM -E - | grep __cplusplus
#define __cplusplus 201402L
```

but libclang, given the same argument list, parses at C++17. So `__cplusplus` -- and with it
`BOOST_CXX_VERSION`, every `BOOST_NO_CXX*` macro, and the availability of `std::pmr` and
friends -- has one value while the splitter decides what to split, and another while the
pieces are compiled.

## Reproduction

```cpp
// g.cpp
#include <cstdio>
#if __cplusplus >= 201703L
int answer() { return 17; }
#else
int answer() { return 14; }
#endif
int main() { std::printf("%d\n", answer()); return 0; }
```

```
$ cpp-splitter clang++ -c -o g.o g.cpp
ld: g.cpp_2_main.cpp:(.text+0x0): multiple definition of `answer()';
    g.o.split/g.cpp_1_answer.o: first defined here
[cpp-splitter] split build failed, falling back to normal compilation

$ tail -4 g.o.split/g.cpp_1_answer.cpp
#if __cplusplus >= 201703L
#line 3 "g.cpp"
int answer() { return 17; }
#endif
```

The piece holds the C++17 body, so libclang took that branch. Its own `#if` is false when
the piece is compiled, so the piece contributes nothing -- and the C++14 body, which the
splitter never saw and therefore never removed, is still sitting in the preamble that every
piece includes.

Adding `-std=gnu++14` to the command line -- making the two agree, whichever value they agree
on -- fixes all seven Boost cases:

| translation unit | symptom without an explicit `-std` |
|---|---|
| `core/test/sv_construct_test_cx2.cpp` | redefinition of `main` |
| `core/test/iterator_test.cpp` | redefinition of `main` |
| `core/test/allocator_pmr_test.cpp` | no member named `pmr` in namespace `std` |
| `core/test/functor_test.cpp` | use of undeclared identifier `g_n` |
| `core/test/as_bytes_test.cpp` | multiple definition |
| `core/test/as_writable_bytes_test.cpp` | multiple definition |
| `core/test/span_deduction_guide_test.cpp` | multiple definition |

Note what that table is: five distinct-looking failures with one cause. Any of them
diagnosed on its own would have produced a wrong answer.

## Description

`do_split()` builds `parse_flags_vec` from `all_flags` (`src/main.cpp:2791`) and hands it to
`clang_parseTranslationUnit2`. The pieces are compiled by invoking the real driver with the
same flags. Neither adds a `-std`, so each side uses its own default and the defaults are not
required to match -- libclang is a library linked into cpp-splitter, built from whatever
clang version the toolchain provides, while the driver is whichever `clang++`/`g++` the build
system chose. They need not even be the same compiler.

Every conditional in the source is then evaluated twice against two different macro
environments. The failures above are the cases where the disagreement is loud. A
disagreement that changes which overload is viable, or which member a class has, would be
just as real and need not fail at all.

## Implementation spec

1. **Ask the driver what it defaults to.** Once per process, run

   ```
   <compiler> -x c++ -E -dM /dev/null
   ```

   and read `__cplusplus`. Map it to the corresponding flag (`199711L` → `-std=c++98`,
   `201103L` → `-std=c++11`, `201402L` → `-std=c++14`, `201703L` → `-std=c++17`, `202002L` →
   `-std=c++20`, `202302L` → `-std=c++23`). Use `-std=gnu++NN` when `__STRICT_ANSI__` is
   absent from the same output, which is how the GNU dialects distinguish themselves.

2. **Only when the command line is silent.** If any argument already begins with `-std=`, or
   is `--std`, the two sides already agree and nothing is added.

3. **Add it to the parse, not to the compile.** The driver's behaviour is the reference: it
   is what the build system asked for, and the object the launcher ultimately produces on
   fallback is compiled with it. Push the flag into `parse_flags_vec` and into the flags used
   for `build_libclang_pch()`, leaving the piece-compile commands untouched.

4. **Cache it.** The probe is one process launch per cpp-splitter invocation, keyed by
   compiler path; it must not run per piece. Failure to probe is not fatal -- fall back to
   adding nothing, which is today's behaviour.

5. **Say so when verbose.** `[cpp-splitter] parse standard: -std=gnu++14 (probed)` makes the
   next disagreement of this kind visible in one line rather than as five unrelated errors.

A cheaper variant -- always pass `-std=c++17` -- is wrong: it makes the parse disagree with
every project that defaults to something else, which is the same bug with a different sign.

## Acceptance Criteria

- The reproduction above splits, links, prints `14`, and the piece's `#if` is the branch the
  compiler actually takes.
- A regression fixture in `test/`: a source with two definitions of one function under
  `#if __cplusplus >= 201703L` / `#else`, compiled with no `-std` on the command line.
- The seven Boost translation units listed above split without falling back, with no `-std`
  added to their commands by hand.
- `CPP_SPLITTER_VERBOSE=1` reports the standard the parse used.

## Outcome

Implemented, and the cause was one step further back than this file assumed: libclang was not
falling back to its own default at all. `build_clang_flags()` put a hardcoded `-std=c++17` in
front of the user's flags, so a command line naming no standard was parsed at C++17 while the
driver compiled at gnu++14. The disagreement was ours, not libclang's -- which does not
change the fix, but does mean it had been deliberate once.

`probe_driver_standard()` now asks the driver what it defaults to and hands libclang that.
The command line still wins where it names a standard, and an unrecognised or unavailable
answer falls back to the `-std=c++17` this used to force, so no project is worse off than
before. The launcher's verbose output names the standard the parse used.

All seven translation units split.

Fixture: `launcher.default_standard`, which needs a driver of its own -- the check is that
split and plain agree, and no `-std` may appear on the command line, which is exactly what
the other fixtures' driver passes.
