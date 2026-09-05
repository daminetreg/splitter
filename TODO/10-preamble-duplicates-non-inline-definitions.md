# 10 — A non-inline definition kept in the preamble is emitted once per split piece

**Severity:** High. Costs a whole translation unit on the Boost example, and the failure is
a duplicate-symbol error at `ld -r` rather than anything pointing at the cause.

**Status: implemented.** The Boost example now splits all 12 of 12 translation units with no
fallbacks. See "Outcome" at the end.

## Motivation

Every split piece begins by including the translation unit's preamble, which is the source
with the split-out function bodies carved out. Anything left in there is therefore compiled
into *every* piece. That is fine for a definition with vague linkage -- an `inline`
function, a template -- because the linker merges the copies. It is not fine for an
ordinary definition with external linkage: each piece emits a strong symbol and `ld -r`
rejects the result.

`libs/filesystem/src/exception.cpp` hits this. `filesystem_error` has a virtual destructor,
and virtual member functions are kept in the preamble on purpose (TODO 05: `override` and
`final` are routinely macros that cannot be stripped textually, so moving a virtual
out-of-line is not safe). But this destructor is `BOOST_FILESYSTEM_DECL`, non-inline, in a
`.cpp` -- a strong definition. The build reports:

```
exception_preamble.h:54: multiple definition of `boost::filesystem::filesystem_error::~filesystem_error()'
exception_preamble.h:54: multiple definition of `boost::filesystem::filesystem_error::what() const'
exception_preamble.h:54: multiple definition of `typeinfo for boost::filesystem::filesystem_error'
```

The `typeinfo` line shows the second half of the problem: the class's key function is the
first non-inline virtual member, so once that lands in the preamble every piece emits the
vtable and type information too.

`exception.cpp` then falls back to plain compilation, which is why it is one of the two
translation units on the Boost example that produce no split objects.

## Reproduction

Eighteen lines, no Boost:

```cpp
// a.cpp
#include <iostream>
namespace demo {
struct Base {
    virtual ~Base();
    virtual int kind() const;
};
Base::~Base() {}                      // virtual, non-inline, external linkage
int Base::kind() const { return 7; }
int one()   { return 1; }             // three more functions, so there are
int two()   { return 2; }             // several pieces to collide with
int three() { return 3; }
}
int main() {
    demo::Base b;
    int n = b.kind() + demo::one() + demo::two() + demo::three();
    std::cout << n << "\n";
    return n == 13 ? 0 : 1;
}
```

```sh
CXX=/usr/local/share/.tipi/clang/4f846ee/bin/clang++
CPP_SPLITTER_NO_SERVER=1 ./build/cpp-splitter "$CXX" -std=c++17 -c -o /tmp/a.o a.cpp
```

```
a.cpp_4_two.cpp:(.text+0x0): multiple definition of `demo::Base::~Base()';
  a.o.split/a.cpp_3_one.o:a.cpp_3_one.cpp:(.text+0x0): first defined here
```

The reproduction needs three things: a virtual member function (so the keep-in-header rule
applies), defined non-inline with external linkage (so the symbol is strong), and at least
two other functions in the file (so more than one piece includes the preamble).

## Description

`prepare_functions()` keeps a definition in the preamble for several good reasons -- it is a
template, a virtual, a constructor in a header, `constexpr`, in an unnamed namespace. Every
one of those cases happens to have vague linkage *except* virtuals defined non-inline in a
`.cpp`, which is the gap.

The preamble is a header included many times. Only definitions that may legally appear in
many translation units belong in it.

### Implementation plan

The preamble currently conflates two kinds of content. Most of it is *declarations* --
classes, enums, typedefs, forward declarations -- which every piece needs and which are
inherently safe to include any number of times. Mixed in with it are *definitions* that
happened to be left behind, and those are only safe to include repeatedly when they have
vague linkage. The fix is to stop mixing them, so that "safe to include N times" becomes a
property of how the preamble is built rather than something each keep rule has to get right
on its own.

Split the preamble into two tiers:

- **Tier one, `<stem>_preamble.h`** -- declarations, plus definitions with vague linkage:
  `inline` functions, templates, `constexpr`, anything with internal linkage. Every piece
  includes it, exactly as today.
- **Tier two, one or more definition headers** -- everything that must appear exactly once.
  Each is included by exactly one piece; tier one keeps a declaration in its place.

This was tried by hand on the reproduction before being written down. Carving
`Base::~Base()` and `Base::kind()` out of the preamble into a second header included by a
single piece takes the build from 21 `multiple definition` diagnostics to none, and the
program links and returns the right answer.

Why this is better than moving the definition out of line, which is what this file
originally proposed:

1. **It covers more than functions.** The same collision happens to namespace-scope
   variables with external linkage, which no amount of out-of-lining addresses -- a variable
   cannot be turned into a split function piece. A three-variable test file produces
   `multiple definition of 'demo::counter'` and `of 'demo::label'` today, and the same
   layering fixes it: 6 diagnostics to none, program runs correctly. Anonymous-namespace
   variables have the same shape and are currently duplicated silently, since internal
   linkage lets them link while giving each object its own copy.
2. **It leaves the source text alone.** TODO 05 keeps virtual members in the preamble
   precisely because `override` and `final` are usually macros that cannot be stripped
   textually. Layering never rewrites the declarator, so that problem never arises and the
   two rules stop conflicting -- no reconciliation needed.
3. **The key-function consequence falls out for free.** The vtable and typeinfo follow the
   first non-inline virtual, so whichever piece includes that definition tier is where they
   are emitted, and it is exactly one object by construction rather than by a rule someone
   has to remember.

### Steps

1. Classify each definition the preamble would carry as vague-linkage or not.
   `clang_Cursor_isFunctionInlined()` and `clang_getCursorLinkage()` answer it for
   functions; `CXCursor_VarDecl` at namespace scope with `CXLinkage_External` and no
   `inline`/`constexpr` is the variable case.
2. Emit non-vague definitions into tier-two headers instead of tier one, leaving a
   declaration behind. For a variable that means an `extern` declaration; for an out-of-line
   member function the class declaration already stands, so nothing need be emitted.
3. Assign each tier-two header to exactly one piece and include it there. With no compilable
   piece to attach to -- a translation unit whose functions are all header-only -- one piece
   has to be synthesised, or the unit left unsplit.
4. Group by dependency rather than one definition per file. Two tier-two definitions that
   reference each other must land in the same header, so partition by strongly-connected
   component of the reference graph; `collect_emitted()` already builds the edges needed.
5. Extend staleness tracking and the content-hash PCH naming, both of which currently assume
   a single preamble path. Tier one stays shareable across pieces and keeps its PCH; tier two
   is included once each and probably should not have one.
6. Keep the original approach -- forcing non-vague definitions out of the preamble and
   solving the macro-spelled `override` problem -- as the fallback if layering hits a
   wrinkle. It is strictly narrower, since it cannot address the variable case.

## Acceptance Criteria

- The reproduction above splits, compiles, links and runs, returning 0.
- `libs/filesystem/src/exception.cpp` links from its split objects on the Boost example, so
  11 of 12 translation units do rather than 10.
- No `multiple definition` diagnostic anywhere in a Boost `filesystem` split build.
- Exactly one object in the build defines `typeinfo for boost::filesystem::filesystem_error`,
  matching an unsplit build.
- A program linked against the resulting library still passes the nine filesystem
  assertions used to check the split library end to end.
- A namespace-scope variable with external linkage, defined in a `.cpp` alongside several
  functions, no longer produces `multiple definition` either. This is the case the
  out-of-lining approach cannot reach, so it is what distinguishes a real fix from a partial
  one.
- Regression fixtures in `test/`, registered with `add_test`: one covering a class with a
  virtual destructor defined non-inline in a `.cpp` alongside several free functions, and one
  covering namespace-scope variables in the same position.

## Outcome

Implemented as the layered preamble, for functions.

- `FunctionInfo` records `is_inlined` (`clang_Cursor_isFunctionInlined`, true for anything
  defined in-class as well as anything marked `inline`) and `external_linkage`.
  `has_vague_linkage()` combines them with the template and unnamed-namespace cases.
- `generate_preamble()` takes an optional `definitions` output. A kept definition without
  vague linkage goes there instead of into the preamble, wrapped in the namespaces it was
  lifted out of, and the preamble keeps a declaration in its place -- nothing for a member,
  which its class already declares, and a forward declaration for a free function.
- `split_unit()` writes that to `<tag>_definitions.h`, which includes the preamble itself,
  and `emit_split_files()` includes it from exactly one piece: the first compilable one.

Verified:

| check | before | after |
|---|---|---|
| reproduction in this file | 21 `multiple definition`, falls back | **0, links, returns 13** |
| Boost translation units linking from split objects | 10 of 12 | **12 of 12** |
| Boost fallbacks | 2 | **0** |
| `multiple definition` anywhere in the Boost build | present | **0** |
| program linked against the split library | 9/9, 0 undefined | **9/9, 0 undefined** |
| `typeinfo for filesystem_error` copies in the archive | -- | **3, same as an unsplit build** |
| tests | 9 | **10** |

`test/preamble_linkage.cpp` is the regression fixture: a virtual destructor and a virtual
member defined non-inline beside three free functions, so that more than one piece includes
the preamble.

This also unblocked `utf8_codecvt_facet.cpp`, which TODO 11 had left failing here for the
same reason, so both remaining translation units were fixed by this one change.

### Not done: namespace-scope variables

Step 1 covers variables as well as functions, and that half is not implemented. A
namespace-scope variable with external linkage left in the preamble is still emitted by
every piece. It does not arise in the Boost example -- hence 12 of 12 -- but the three-line
test case in the plan above still reproduces it, and an anonymous-namespace variable is
still duplicated silently, one copy per object.

Doing it needs the harvest to record `CXCursor_VarDecl` at namespace scope with its extent,
which the visitor currently ignores entirely; the tier-two machinery it would feed into now
exists. The declaration left behind is `extern`.

Steps 4 and 5 of the plan are also untouched and have not been needed: no two tier-two
definitions have yet had to reference each other, and one definitions header per unit has
not disturbed staleness tracking or the PCH naming.
