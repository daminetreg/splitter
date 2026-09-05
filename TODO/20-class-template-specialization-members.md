# 20 — A member of an explicit class-template specialization loses its template arguments

**Severity:** Medium. 2 of the 62 fallbacks in the wider Boost run
(`container/src/monotonic_buffer_resource.cpp` and `pool_resource.cpp`, both through
`boost/intrusive/detail/math.hpp`).

## Motivation

A member of a class template is kept in the header: `in_class_template` is set when the
semantic parent is a `ClassTemplate` or a `ClassTemplatePartialSpecialization`, because an
out-of-line definition would have to repeat the template header, which a split `.cpp` cannot
do. A *full* explicit specialization is a different cursor kind and does not match, so its
members are split -- and then qualified with the name libclang reports for the parent, which
is the template's name with no arguments.

```cpp
inline auto builtin_clz_dispatch::call(unsigned long n)      // written by the splitter
       -> unsigned long {  return (unsigned long)__builtin_clzl(n); }
```

```
math.hpp:103:13: error: 'builtin_clz_dispatch' is not a class, namespace, or enumeration
math.hpp:90:11: note: 'builtin_clz_dispatch' declared here
   struct builtin_clz_dispatch;
```

The definition needed to say `builtin_clz_dispatch<unsigned long>::call`.

## Reproduction

```cpp
// e.hpp
#pragma once
template <class U> struct clz_dispatch;
template <> struct clz_dispatch<unsigned long> {
    static unsigned long call(unsigned long n) { return n + 1; }
};
```

```cpp
// e.cpp
#include "e.hpp"
int main() { return clz_dispatch<unsigned long>::call(1) == 2 ? 0 : 1; }
```

```
$ cpp-splitter clang++ -I. -c -o e.o e.cpp
e.hpp:4:13: error: 'clz_dispatch' is not a class, namespace, or enumeration
[cpp-splitter] split build failed, falling back to normal compilation
```

## Description

The scope chain is built in `visitor()` (`src/main.cpp:436`):

```cpp
std::string pname = cx_to_string(clang_getCursorSpelling(parent_cursor));
```

For `template <> struct clz_dispatch<unsigned long>` libclang reports the parent as a plain
`CXCursor_StructDecl` -- not `ClassTemplate`, not `ClassTemplatePartialSpecialization` -- and
`clang_getCursorSpelling` gives `clz_dispatch`. The template arguments are lost, and every
consumer of `scope_chain` inherits the loss: the qualification prefix in `prepare_functions()`
that builds `outlined_body`, and `wrap_in_namespaces()`.

Unlike a class template, a full specialization *can* have its members defined out of line
without a template header:

```cpp
template <> struct A<int> { void f(); };
void A<int>::f() {}          // legal, no template<> prefix
```

so the definitions are movable. Only the name is wrong.

## Implementation spec

1. **Detect the parent.** A parent cursor is a full explicit or implicit specialization when
   `clang_getSpecializedCursorTemplate(parent_cursor)` is not null. Test that before taking
   the spelling.

2. **Use the specialized name.** `clang_getCursorDisplayName(parent_cursor)` returns the name
   with its argument list (`clz_dispatch<unsigned long>`). Take it when it contains a `<`;
   store it in the `ScopeEntry` as the name to qualify with, keeping the bare spelling for
   anything that needs an identifier rather than a qualified-id (nothing does today, but
   `make_static_mangled_name()` would).

3. **Refuse when the name is not usable.** If the display name has no `<`, or contains a
   type printed as `(anonymous ...)` or `(lambda ...)`, keep the definition in the header:
   a name that cannot be written cannot qualify anything. Add a `keep_reason()` case,
   `"member of a specialization whose name cannot be written"`, so the generated piece says
   why rather than falling into the catch-all.

4. **Implicit instantiations are already excluded.** A member of `A<int>` where `A<int>` is an
   implicit instantiation is not a definition written in this file, so the visitor never sees
   it as one. No extra guard is needed, but the fixture should include one so that stays
   true.

The change is local to the scope-chain loop plus the `ScopeEntry` struct; the qualification
prefix built from `scope_chain` then comes out right everywhere without further edits.

## Acceptance Criteria

- The reproduction above splits without falling back, and the program returns 0.
- A regression fixture in `test/`: a class template, one full explicit specialization with a
  member function defined in the class body, and a use that instantiates the primary template
  as well, so both paths are exercised.
- `boost/intrusive/detail/math.hpp` splits, and the two Boost.Container translation units no
  longer fall back.
- The generated definition reads `clz_dispatch<unsigned long>::call`, not `clz_dispatch::call`
  -- checked by grepping the piece, not only by the fact that it compiles.

## Outcome

Implemented. Where the semantic parent is a specialization -- `clang_getSpecializedCursorTemplate`
is non-null -- the scope chain takes `clang_getCursorDisplayName`, which carries the argument
list. A display name that cannot be written -- no `<`, or a type printed as `(anonymous ...)`
or `(lambda ...)` -- keeps the definition in the header with a `keep_reason()` of its own.

Both Boost.Container translation units split.

Fixture: `test/specialization_member_header.hpp`, which pairs two full specializations with a
member of the primary template that must still be kept.
