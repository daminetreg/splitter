# 17 — A variable defined in the source is copied into every split piece

**Severity:** High. 31 of the 62 fallbacks in the wider Boost run, by far the largest single
cause, and the only one whose failure mode is a link error rather than a compile error.

`./classify-fallbacks.sh` counts 34 in this bucket, because it groups by symptom. Three of
those are TODO 19 wearing this symptom: a standard mismatch selects a different preprocessor
branch, and the branch it selects happens to define a variable twice. Fixing TODO 19 takes
them out of the bucket; the 31 that remain are this defect.

## Motivation

The preamble is the translation unit's own source with the function bodies carved out.
Everything else is carried through verbatim -- including namespace-scope variable
definitions and out-of-class static data member definitions. Every split piece includes the
preamble, so a definition like

```cpp
namespace demo { int counter = 0; }
```

is emitted once per piece, and `ld -r` rejects the result.

This is exactly the problem the definitions header solves for functions
(`has_vague_linkage()` at `src/main.cpp:510`, and the `definitions` out-parameter of
`generate_preamble()`), applied to nothing but functions. Variables were never considered.

## Reproduction

```cpp
// a.cpp
#include <cstdio>
namespace demo { int counter = 0; }
int bump()  { return ++demo::counter; }
int drain() { return demo::counter--; }
int main() { bump(); drain(); std::printf("%d\n", demo::counter); return 0; }
```

```
$ cpp-splitter clang++ -c -o a.o a.cpp
ld: a.o.split/a.cpp_2_drain.o:(.bss+0x0): multiple definition of `demo::counter';
    a.o.split/a.cpp_1_bump.o:(.bss+0x0): first defined here
ld: a.o.split/a.cpp_3_main.o:(.bss+0x0): multiple definition of `demo::counter';
    a.o.split/a.cpp_1_bump.o:(.bss+0x0): first defined here
cpp-splitter: relocatable link failed using 'ld'
[cpp-splitter] split build failed, falling back to normal compilation
```

Three pieces, three copies of one variable. In Boost the shapes are the same:
`boost::thread_detail::future_error_category_var` (namespace scope),
`X::instances` and `type::count` (out-of-class static data members), `s_mx` (a namespace-scope
mutex in a test).

## Description

`generate_preamble()` walks the source and replaces each function's extent with a
declaration or with nothing, copying every byte between the extents unchanged. A variable
definition is never inside a function extent, so it is always copied.

Which variables are safe to copy is the same question `has_vague_linkage()` answers for
functions:

| kind | linkage | safe in the preamble |
|---|---|---|
| `inline` / `constexpr` variable | vague | yes |
| `const` at namespace scope | internal (C++) | yes |
| `static`, or in an unnamed namespace | internal | yes, but see below |
| a template's static data member | vague | yes |
| everything else | external | **no** |

The internal-linkage case deserves a note: it links, so it never shows up as a fallback, but
each piece gets its *own* copy of the variable. A `static int calls = 0;` counted by two
functions in the same translation unit now counts in two separate objects. Nothing reports
this -- it is a silent behaviour change, and worth fixing at the same time and for the same
reason, not because the linker complains.

## Implementation spec

1. **Harvest variables alongside functions.** Extend `visitor()` to record
   `CXCursor_VarDecl` cursors that are definitions (`clang_isCursorDefinition`) and whose
   semantic parent is a namespace or the translation unit, plus out-of-class static data
   member definitions (a `VarDecl` whose lexical parent is not its semantic parent). Store
   file, extent offsets, USR, name, scope chain and linkage in a `VariableInfo` alongside
   `FunctionInfo`; the two only need to share the extent fields that `generate_preamble()`
   consumes.

2. **Decide where each goes.** A variable is left in the preamble when
   `clang_getCursorLinkage()` is not `CXLinkage_External`, or when its declaration text
   carries `inline` or `constexpr` (read from `clang_getCursorPrettyPrinted`, as
   `is_constexpr` already does for functions, because both are routinely macros). Everything
   else moves to the definitions header.

3. **Leave a declaration behind.** For an out-of-class static data member, nothing: the class
   already declares it. For a namespace-scope variable, `extern <declaration>;` -- the text
   up to the initialiser, which is the source text truncated at the first `=`, `(` or `{` at
   bracket depth zero before the terminating `;`, found on `blank_code_noise()` output so
   that a `=` inside a string or comment is not mistaken for the initialiser. If no such
   truncation can be made confidently, keep the variable in the preamble: less splitting,
   nothing broken.

4. **Order.** The definitions header already includes the preamble, so a moved variable can
   refer to any type the preamble declares. A moved variable's *initialiser* may call a
   function that also moved -- both end up in the same file, in source order, which is the
   order that already worked.

5. **Internal linkage.** Same treatment as functions: they are kept, but a note in the
   generated preamble explaining that each piece gets its own copy would make the behaviour
   discoverable. Renaming them the way `build_static_rename_map()` renames static functions
   and moving them to the definitions header is the complete fix, and can follow separately.

The whole change is confined to `generate_preamble()` and the visitor. The definitions
header, its single-consumer piece, and the pruning logic all already exist and need no
change.

## Acceptance Criteria

- The reproduction above splits, links and prints `0`.
- A regression fixture in `test/`: one translation unit with a namespace-scope variable, an
  out-of-class static data member, a `const` and a `constexpr` variable, split across at
  least three pieces, whose program checks the values rather than only linking.
- The wider Boost run loses the 31 fallbacks in this class; `grep -c 'multiple definition'`
  over its log is zero.
- The filesystem example still splits 12 of 12 with no fallbacks and scores 9/9.

## Outcome

Implemented. Variables are harvested alongside functions, and one with external linkage that
is neither `inline` nor `constexpr` nor inside a template moves to the definitions header,
with an `extern` left at the position it occupied.

Three shapes turned up only once variables started moving, all of them the same mistake --
taking more of a declaration than the variable owns -- and all of them regressions the wider
Boost run caught rather than the fixture:

* A `template<...>` prefix sits outside the cursor's extent, the same way an explicit
  specialization's `template< >` does. Moving `template<int M> spinlock
  spinlock_pool<M>::pool_[41] = {...};` stranded the prefix and carried a definition into the
  definitions header without the template header it needs.
* `struct foo { ... } f;` declares a type and a variable together. Only the declarator moves
  now; the type stays as ordinary text. Taking the whole declaration also swallowed the
  member functions defined in that type, putting back definitions that had been split while
  their pieces were compiled anyway.
* A variable range that overlaps a function range is dropped as a safety net. Folding
  overlapping ranges and emitting the union verbatim is right for a macro expansion and wrong
  here.

The silent half is unchanged: internal-linkage variables still get a copy per piece, so a
`static int calls = 0;` shared by two functions still counts in two objects.

**Both halves of the sentence that used to follow were wrong, and are corrected here.** It
said moving them "needs the rename that TODO 23 declined to generalise, and nothing observable
in this corpus depends on it".

TODO 23 declined a rename map shared *between* translation units, which is a problem only for
a definition in a **header**: two units split the same header separately and would have to
agree on the mangled name. A `static` variable in the unit's own source has no such problem --
`build_static_rename_map()` is already per-unit and already sufficient. The `.cpp` case was
never blocked by TODO 23; it was deferred by step 5 of this file's own spec, which says the
rename "can follow separately".

And something observable does depend on it. `example/static-init-order/` is three files and no
Boost: the registrar it declares `static` is copied into every piece, so the registry it fills
ends up with one entry per piece instead of one. That was written after this outcome was, and
it falsifies it.

The `.cpp` half is now TODO 26. Variables in headers stay out of scope, because those really do
need what TODO 23 declined.

Fixture: `test/preamble_variables_main.cpp`, which carries all nine kinds -- moved,
`const`, `constexpr`, `inline`, `static`, a static data member, a multi-declarator
declaration, an array, a class-template member and an inline type definition -- and checks
the values rather than only linking.
