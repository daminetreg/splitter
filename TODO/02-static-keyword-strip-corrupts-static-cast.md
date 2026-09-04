# 02 — Stripping `static` corrupts `static_cast` inside function bodies

**Severity:** Critical — because it silently rewrites user code into something that
means something else, not because of error volume.

**Status: implemented and verified.** See "Outcome" at the end of this file.

An earlier revision of this file claimed this defect caused 1872 of the errors in the
Boost run. That was wrong: those errors are class-member extraction failures (TODO 05).
Measured against the build log, the `static_cast` corruption accounted for 2 reported
errors — clang stops early once a body is corrupted — while aborting the two
translation units that hit it.

## Motivation

When splitting a function that libclang reports as having internal linkage, the tool
tries to remove the `static` keyword from the extracted definition. It does so by
searching the **entire function body** for the literal string `"static"` and erasing the
first occurrence. If the definition does not actually begin with the `static` keyword,
the search lands on the first `static_cast` in the body and turns it into `_cast`.

This is not a rare edge case. Any function with internal linkage that has no literal
`static` keyword hits it — most commonly a function in an anonymous namespace, or an
`inline` function at namespace scope in a `.cpp`. Boost uses both heavily.

Confirmed instance, `libs/filesystem/src/path.cpp:112`:

```cpp
inline size_type find_separator(const char* p, size_type size) noexcept
{
    const char* sep = static_cast< const char* >(std::memchr(p, '/', size));
```

becomes, in `path_1_find_separator.cpp`:

```cpp
inline size_type __static_path__find_separator(const char* p, size_type size) noexcept
{
    const char* sep = _cast< const char* >(std::memchr(p, '/', size));
```

`find_separator` is `inline` inside an anonymous namespace: internal linkage, no
`static` keyword anywhere in its text. Same failure in `unique_path.cpp`
(`fill_random_dev_random`, `fill_random_getrandom`, `system_crypt_random`).

This class of bug is the worst kind the splitter can have: it produces plausible-looking
code that is wrong. Here it happens to fail to compile, but a textual edit that lands on
a different token could just as easily compile and change behaviour.

## Description

Two identical copies of the offending code exist — `src/main.cpp` ~line 1379 (inside
`do_split_with_cache`) and ~line 1636 (inside `do_split`):

```cpp
std::string body = fn.body;
if (fn.is_static) {
    size_t spos = body.find("static");      // searches the whole body
    if (spos != std::string::npos) {
        size_t after = spos + 6;
        while (after < body.size() && body[after] == ' ')
            ++after;
        body.erase(spos, after - spos);
    }
}
```

Two independent defects:

1. **Unbounded search.** The scan covers the whole body instead of just the declaration
   that precedes the opening brace.
2. **No token boundary.** `after = spos + 6` steps past `static` and then only skips
   spaces. For `static_cast` there are no spaces to skip, so exactly `"static"` is
   erased and `_cast` is left behind. A correct check requires the next character to be
   whitespace (and the previous character to not be an identifier character).

`fn.is_static` comes from linkage, not syntax (`src/main.cpp:194`):

```cpp
info.is_static = (clang_getCursorLinkage(cursor) == CXLinkage_Internal);
```

so "internal linkage" and "the text starts with `static`" are simply different things.
The strip must be a best-effort no-op when the keyword is absent.

The same pattern — unbounded `find` of a keyword — also appears in
`generate_forward_decl()` for `"inline "` and `"static "`. Those operate on the
signature only so the blast radius is smaller, but they are wrong for the same reason
(a parameter type or default argument containing the substring will be mangled).

### Implementation plan

1. Add one shared helper, used by all sites:

   ```cpp
   // Erase a leading `static` / `inline` specifier from a declaration, if present.
   // Only looks at the declaration prefix (text before the first top-level `{`),
   // and only matches whole tokens.
   static std::string strip_decl_specifier(const std::string& text,
                                           const std::string& keyword);
   ```

2. Bound the search to the declaration prefix. `extract_source_signature()` already
   implements the brace scan that finds the first top-level `{`; factor that scan out
   and reuse it rather than duplicating it a third time.
3. Match whole tokens only: the character before the match must not be alphanumeric or
   `_`, and the character after must be whitespace. Skip matches inside string
   literals, character literals and comments — a minimal token scanner is enough, and
   is needed anyway for TODO 03.
4. Make "keyword not found" an explicit, silent no-op with a comment explaining that
   `is_static` is linkage-derived and internal linkage often has no keyword.
5. De-duplicate: `do_split()` and `do_split_with_cache()` contain two near-identical
   copies of the whole split-emission block. Extract the shared body-emission logic into
   one function so fixes land once. This is a prerequisite for TODO 03 as well.
6. Harden `generate_forward_decl()`'s `"inline "` / `"static "` handling with the same
   helper.

## Acceptance Criteria

- Grepping the split output of a Boost `filesystem` build for a `_cast` token not
  preceded by an identifier character returns nothing:
  ```sh
  grep -rnE '(^|[^A-Za-z_])_cast[[:space:]]*<' build-split --include='*.cpp'
  ```
- ~~`libs/filesystem/src/path.cpp` and `libs/filesystem/src/unique_path.cpp` compile from
  their split pieces and link via `ld -r` without falling back.~~ **Not reachable from
  this item alone** — both translation units are blocked by other root causes once the
  corruption is gone (see Outcome).
- Unit/regression fixtures covering:
  - an `inline` function in an anonymous namespace whose body contains `static_cast`
    (must round-trip unchanged apart from the rename);
  - a genuine `static void f()` at namespace scope (the keyword must still be removed);
  - a function whose body contains the string literal `"static "` (must be untouched);
  - a function with a local `static` variable (must be untouched).
- No behavioural change for functions with external linkage.

## Outcome

Implemented in `src/main.cpp`:

- New lexical helpers `is_ident_char()`, `blank_code_noise()`, `decl_prefix_end()` and
  `strip_decl_specifier()`. `blank_code_noise()` returns an offset-preserving copy of the
  text with comments, string literals, character literals and raw string literals blanked
  out, so token searches never fire inside them; digit separators (`1'000`) are
  distinguished from character literals by inspecting the preceding token.
- `strip_decl_specifier()` bounds the search to the declaration prefix (text before the
  parameter list, or failing that the body) and requires whole-token matches, so `static`
  can no longer match inside `static_cast`. Absence of the keyword is a silent no-op,
  which is the normal case for internal linkage arising from an anonymous namespace.
- Both copies of the naive strip are gone, and `generate_forward_decl()` now uses the
  same helper for `inline` and `static`.
- Step 5 of the plan (de-duplication) is done: the two ~150-line emission blocks in
  `do_split()` and `do_split_with_cache()` were byte-identical apart from a lambda name
  and are now a single `emit_split_files()`.

Verified:

- `test/static_specifier.cpp` covers all four required cases and round-trips correctly;
  it splits, compiles, links and runs (exit 0).
- Boost `filesystem` build: zero corrupted `_cast` tokens in the split output
  (`grep -rnE '(^|[^A-Za-z_])_cast[[:space:]]*<'` returns nothing, was 4 files).
- The two translation units that previously linked from split objects
  (`find_address_sse2`, `find_address_sse41`) still do.

Not fixed by this item — what each blocked translation unit now fails on:

- `unique_path.cpp` — TODO 03 only: `use of undeclared identifier 'fill_random_dev_random'`
  from the un-renamed preamble. This one should go green with TODO 03 alone.
- `path.cpp` — TODO 04: the `atomic_ref.hpp` basename collision breaks its PCH, and the
  `path.cpp` / `path.hpp` stem collision deletes its split files outright.

Fallback count across the Boost build is unchanged at 9 of 12 translation units; the
remaining blockers are TODO 03, 04 and 05.
