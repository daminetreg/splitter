# 34 — `inline` inserted into the middle of an alias template

**Severity:** High. The splitter rewrote a header into something that does not compile, and it
did it to Boost.Mp11, which most of Boost includes. Five Boost.Spirit units fell back; a
fallback writes no split cache, so each re-did its whole split and compiled plain on *every*
build (`TODO/31`).

**Status: fixed.** The cause was not what this file first said it was, and the gap between the
two is the useful part — see "What it actually was".

## The symptom

Five of the 277 Boost.Spirit test units fell back in the cmake-re build:

```
spirit_test_qi_range_run   spirit_test_qi_regression_transform_assignment
spirit_test_qi_tst         spirit_test_support_utree
spirit_test_x3_container_support
```

all with the same errors from `boost/mp11/algorithm.hpp`:

```
algorithm.hpp:1295:16: error: use of alias template 'detail::mp_pairwise_fold_impl' requires
                              template arguments
algorithm.hpp:1295:1:  error: type name does not allow function specifier to be specified
```

Mp11 writes this on one line:

```cpp
template<class L, class Q> using mp_pairwise_fold_q =
    mp_eval_if<mp_empty<L>, mp_clear<L>, detail::mp_pairwise_fold_impl, L, Q>;
```

and the rewritten copy read:

```cpp
template<class L, class Q> using mp_pairwise_fold_q = mp_eval_if<mp_empty<L>, mp_clear<L>,
inline detail::mp_pairwise_fold_impl, L, Q>;
```

`inline` inserted into the middle of a template argument list.

## What this file first concluded, and why it was wrong

The original diagnosis was that an alias template was being harvested as if it were a variable,
and that `inline_insertion_point()` returned an offset it should have refused. The spec said to
exclude `CXCursor_TypeAliasDecl` / `CXCursor_TypeAliasTemplateDecl` / `CXCursor_TypedefDecl`
and to make the insertion point fail closed.

**Both of those would have been treatments for a symptom.** The give-away was recorded here at
the time and not followed up: `./benchmark-spirit-tests.sh` builds the same 277 targets from
the same sources and reports 0 fallbacks. Nothing about alias templates differs between the two
builds. Something about the *invocation* did.

Splitting `libs/spirit/test/qi/tst.cpp` by hand, with the same flags, produced a clean header.
Splitting it **chained behind a launcher** — the way cmake-re invokes it — reproduced the
corruption immediately.

## What it actually was

`run_as_launcher()` set the program that the system-include and default-standard probes run
against straight from `argv[1]`:

```cpp
std::string compiler = argv[1];
g_compiler = compiler;
```

cmake-re composes `CMAKE_CXX_COMPILER_LAUNCHER` as `<cpp-splitter>;tipi-compiler-driver`, so
the process is invoked as `cpp-splitter tipi-compiler-driver clang++ <flags>` and **argv[1] is a
launcher, not a compiler**. The splitter already knew that elsewhere — `chained_behind_driver`
computes it — but 86 lines further down, long after `g_compiler` was set.

Probing a launcher does not work:

```
$ tipi-compiler-driver -x c++ -E -dM /dev/null
execve failed: No such file or directory
```

`detect_system_includes()` and `probe_driver_standard()` both come back empty. libclang then
parses a translation unit **without the compiler's system include paths**, which is not the
translation unit the compiler sees. The harvest comes out of that AST, and the damage surfaces
wherever the resulting offsets happen to land — in this case inside an alias template in a
header nobody was looking at.

## The fix

```cpp
g_compiler = (compiler == "tipi-compiler-driver" && argc > 2) ? argv[2] : compiler;
```

One line, at the point where `g_compiler` is set rather than where the chaining is later
noticed.

Two further defects fell out of writing the fixture, both mine and both recent:

- The relocatable link was handed to `tipi-linker-driver` **unconditionally** when chained. A
  build with the compiler driver but no linker driver therefore fell back on every unit. It is
  now used only when the driver is actually there.
- `which_on_path()` tested for existence. The tipi image ships every `tipi-*-driver` as
  `-rwxrw-r--` owned by `tipi`, so a build running as another uid finds the file and cannot
  execute it: the link died with `Permission denied` and fell the unit back. It now requires
  `X_OK`.

## The fixture, and why its first version was worthless

`launcher.chained_behind_driver` runs the splitter behind a stub launcher — `exec "$@"`, which
is what the real driver does with a compile command and exactly what it cannot do with
`-x c++ -E -dM /dev/null`, so the probe fails there the same way.

The first version asserted "no fallback" and **passed without the fix**. The unit includes only
`<string>`, and clang finds that through its own resource directory whether or not the splitter
asked the compiler where the system headers are. The bug is silent on small code and loud on
Boost.Mp11, which is how it survived long enough to be misfiled.

It now asserts on the cause instead of the symptom: `parse standard: ... (probed)` appears only
when the splitter got an answer back from something that really is a compiler. Verified — 0
such lines without the fix, 1 with.

## Verified

- 33/33 ctest.
- The Boost.Spirit suite through cmake-re on RBE: **0 fallbacks on every row**, where the same
  benchmark previously reported 5 on the cold split build and 2 on the body edit.
- `boost/mp11/algorithm.hpp` comes out of the mirror byte-identical to the original.

## The lesson worth keeping

A defect report named after its symptom will send you to the wrong file. This one said "alias
template" in its title and its spec, and the fix is one line about `argv[1]` in the launcher.
The evidence that should have redirected it was in the file from the first draft — the same
sources split cleanly under a different invocation — recorded as an open question rather than
followed.
