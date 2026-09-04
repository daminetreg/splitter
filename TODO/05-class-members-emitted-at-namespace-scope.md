# 05 — Class member functions are emitted at namespace scope

**Severity:** High. This is the bulk of the work needed to make header splitting useful,
and header splitting is where most of the split volume comes from.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

Member functions defined inside a class body are extracted verbatim and written into a
split `.cpp` wrapped only in their enclosing **namespaces** — the class scope is
dropped. The extracted text is then an invalid non-member definition.

From the Boost `filesystem` run:

| error | count |
|---|---|
| `non-member function cannot have 'const' qualifier` | 30 |
| `variable type '...' is an abstract class` | 26 |
| `only constructors take base initializers` | 21 |
| `invalid use of 'this' outside of a non-static member function` | 12 |

This matters disproportionately because header splitting dominates the output: of 4592
split `.cpp` files generated for 12 translation units, the large contributors are all
header classes — `boost/filesystem/path.hpp` (154 functions),
`boost/filesystem/directory.hpp` (137), `boost/filesystem/operations.hpp` (110),
`boost/scope/unique_resource.hpp` (70), `boost/io/ios_state.hpp` (53),
`boost/smart_ptr/intrusive_ptr.hpp` (41), `boost/iterator/iterator_facade.hpp` (36).
Until member functions are handled, essentially none of that output is compilable.

## Description

Two places drop class scope:

`wrap_in_namespaces()` (`src/main.cpp:416`) stops at the first non-namespace entry:

```cpp
for (const auto& entry : scope_chain) {
    if (entry.kind == ScopeKind::Namespace)
        ns_names.push_back(entry.name);
    else
        break;                       // class scope is discarded
}
```

`generate_forward_decl()` (`src/main.cpp:292`) bails out entirely for members (line 302):

```cpp
if (is_class_method)
    return "";
```

So `T name(args) const { ... }`, lifted out of a class body, is emitted at namespace
scope. `const` is now illegal, `this` is unavailable, and a constructor's
mem-initializer list parses as a base-initializer on a free function.

The correct output is an out-of-line definition: `T Class::name(args) const { ... }`,
wrapped in the enclosing namespaces only, with the class qualification inserted before
the function name and in-class-only specifiers removed.

### Implementation plan

1. **Decide what is splittable.** Extend `should_keep_in_header()` — currently just
   `return fn.is_template;` — to also keep in the header any function that cannot be
   defined out-of-line in a `.cpp`:
   - any member of a class template or of a nested class inside a class template
     (`ScopeKind::Class` entry whose cursor kind is `CXCursor_ClassTemplate`);
   - members of local or anonymous classes;
   - defaulted/deleted members;
   - anything `constexpr` or `consteval` that callers must see (or keep them in the
     header unconditionally for now).
   `ScopeEntry` currently records only a name and a Namespace/Class discriminator; it
   needs to carry enough cursor information to answer these questions. Widen it to
   record the cursor kind, and whether the entity is a template.
2. **Build the out-of-line signature from libclang, not from text.** Rather than
   patching the source text, reconstruct the declarator: return type, fully qualified
   `A::B::` prefix, function name, parameter list, and trailing qualifiers
   (`const`, `&`/`&&`, `noexcept`, trailing return type). Use `extract_source_signature()`
   only as a fallback and to preserve the original parameter spelling where the printed
   type would be lossy (e.g. typedefs, default arguments).
3. **Strip in-class-only specifiers** from the emitted definition: `virtual`, `static`,
   `explicit`, `friend`, `inline` where inappropriate, and default arguments (which may
   appear only on the declaration).
4. **Preserve constructor mem-initializer lists** — they are legal on the out-of-line
   definition and must be carried through with the `Class::Class(...)` form.
5. **Emit only the enclosing namespaces.** Change `wrap_in_namespaces()` to take the
   already-qualified definition and wrap it in the leading namespace run, and to reject
   (assert) a scope chain where a Namespace entry follows a Class entry.
6. **Forward declarations.** `generate_forward_decl()` returning `""` for members is
   acceptable once the class definition itself remains in the preamble — the member is
   already declared there. Verify this holds and document it, rather than leaving the
   early return unexplained.
7. Sequence this after TODO 02 and TODO 03; both touch the same emission path, and this
   change is much easier on a de-duplicated `do_split` / `do_split_with_cache`.

## Acceptance Criteria

- Splitting `boost/filesystem/path.hpp` produces compilable `.cpp` pieces for its
  non-template member functions, including at least one `const` member and one
  constructor with a mem-initializer list.
- The four error classes above no longer appear in a Boost `filesystem` split build log.
- Members of class templates are retained in the preamble and are not emitted as
  compilable pieces (`compilable_files` must not contain them).
- Regression fixtures covering: a `const` member; a constructor with a base and member
  initializer list; a destructor; a `virtual` override; a `static` member function; a
  member of a nested class; a member of a class template (must be kept in header).
- Symbols emitted from split member definitions match those from an unsplit build:
  compare `nm --defined-only` output for a fixture object built both ways.

## Outcome

Implemented in `src/main.cpp`:

- `FunctionInfo` gained `defined_in_class`, `in_class_template`, `in_anonymous_class`,
  `keep_in_header`, `member_decl` and `outlined_body`, filled by a new
  `prepare_functions()` pass that runs right after the AST visit. `should_keep_in_header()`
  now just reads the precomputed decision.
- Supporting helpers: `definition_decl_end()` (finds the ':' of a member-initialiser list
  or the '{' of the body at top level), `find_declarator()`, `strip_virt_specifiers()`,
  `strip_default_args()`, `skip_leading_attributes()`, `has_trailing_return()`.
- A member defined inside its class body is rewritten to the out-of-line form: `virtual`,
  `static`, `explicit`, `friend`, `override` and `final` removed, default arguments
  removed, and the declarator qualified with the full class chain (`Widget::Inner::`).
  Member-initialiser lists are carried through unchanged.
- `generate_preamble()` leaves a declaration inside the class where the definition was.
  Without it the out-of-line definition matches nothing and the member vanishes from the
  type.
- **Return types are moved to a trailing return type** (`auto C::f() const -> R`). A
  leading return type is looked up in the enclosing namespace once out-of-line, so
  class-scoped names such as `result_type` or `iterator` stop resolving; a trailing return
  type is looked up in class scope, which fixes all of them at once. The type text comes
  from libclang rather than the source span, because the span also contains function
  specifiers that are routinely macros (`BOOST_FORCEINLINE`) and cannot be told apart from
  a type name textually.
- Kept in the header: templates, members of class templates, members of unnamed classes,
  definitions with no body (`= default`, `= delete`), `constexpr`/`consteval`, deduced
  return types, and anything whose declarator could not be located.

One trap worth recording: a definition already written out-of-line (`void path::foo() {}`)
also has a class as its *semantic* parent, so the first version of this change emitted a
stray `void path::foo();` at namespace scope for every one of them -- 2900 instances of
`out-of-line declaration of a member must be a definition`. The test is the *lexical*
parent (`clang_getCursorLexicalParent`), which is the class only when the body really is
inside the class body.

Verified on the Boost `filesystem` build:

| check | before | after |
|---|---|---|
| `non-member function cannot have '...' qualifier` | 101 | **0** |
| `only constructors take base initializers` | 69 | **0** |
| `invalid use of 'this' outside...` | 46 | **0** |
| `variable type '...' is an abstract class` | 78 | **0** |
| total `error:` lines | 10869 | **2050** |
| compilable `boost/filesystem/path.hpp` pieces that compile | 0 / 104 | **100 / 104** |

`test/member_functions.cpp` covers a const member, a constructor with base and member
initialisers plus a default argument, a destructor, a `virtual` override, a static member
function, a member of a nested class, and members of a class template (correctly kept
header-only). It splits, compiles, links and runs. Symbols match an unsplit build exactly:
27 defined symbols each, no differences, once compiler-internal exception-table labels are
excluded.

Still 2 of 12 translation units link from split objects. The remaining blockers are no
longer member extraction:

- The 4 remaining `path.hpp` pieces and both header-dependency failures fail on missing
  declaration context in split headers -- `no template named 'basic_string_view' in
  namespace 'std'`, and an incomplete `error_condition` return type in
  `std_category_impl.hpp`. These belong with **TODO 06**.
- **TODO 03** still blocks `unique_path.cpp`, unchanged.
- **TODO 08** (new) makes every incremental rebuild worse than the first.
