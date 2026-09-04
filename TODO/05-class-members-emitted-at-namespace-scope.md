# 05 — Class member functions are emitted at namespace scope

**Severity:** High. This is the bulk of the work needed to make header splitting useful,
and header splitting is where most of the split volume comes from.

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
