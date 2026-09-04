# 10 — A non-inline definition kept in the preamble is emitted once per split piece

**Severity:** High. Costs a whole translation unit on the Boost example, and the failure is
a duplicate-symbol error at `ld -r` rather than anything pointing at the cause.

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

1. Add the missing condition to the keep decision: a definition may stay in the preamble
   only if it has vague linkage, i.e. it is `inline`, a template, `constexpr`, or has
   internal linkage. Otherwise it must be split out even when another rule wanted to keep
   it. libclang answers this directly with `clang_Cursor_isFunctionInlined()` and
   `clang_getCursorLinkage()`.
2. That collides with TODO 05's reason for keeping virtuals, so those two rules have to be
   reconciled rather than stacked. A virtual member defined non-inline in a `.cpp` has to be
   moved out-of-line, which means solving the macro-spelled `override` / `final` problem for
   that case -- most likely by taking the declarator from libclang rather than from the
   source text, as the trailing-return-type rewrite already does.
3. Consider the narrower alternative: emit such definitions into exactly one designated
   piece rather than into the preamble, and leave a declaration in the preamble. That keeps
   the text unmodified, sidestepping the `override` problem entirely, and is closer to what
   the rest of the splitter already does.
4. Whichever route, handle the key-function consequence: the vtable and typeinfo follow the
   first non-inline virtual, so wherever that definition lands is where they will be
   emitted, and it must be exactly one object.

## Acceptance Criteria

- The reproduction above splits, compiles, links and runs, returning 0.
- `libs/filesystem/src/exception.cpp` links from its split objects on the Boost example, so
  11 of 12 translation units do rather than 10.
- No `multiple definition` diagnostic anywhere in a Boost `filesystem` split build.
- Exactly one object in the build defines `typeinfo for boost::filesystem::filesystem_error`,
  matching an unsplit build.
- A program linked against the resulting library still passes the nine filesystem
  assertions used to check the split library end to end.
- Regression fixture in `test/`, registered with `add_test`, covering a class with a virtual
  destructor defined non-inline in a `.cpp` alongside several free functions.
