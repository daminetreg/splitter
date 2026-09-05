# 15 — Macros that expand to nothing are left behind in generated files

**Severity:** Low. Cosmetic only. An earlier revision of this file claimed it was a latent
ABI bug; that was wrong, and the correction is recorded below because the reasoning is worth
keeping.

## What happens

Generated preambles contain lines like this:

```cpp
//  normal  --------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL 


BOOST_FILESYSTEM_DECL 
```

`path_preamble.h` has 43 occurrences of `BOOST_FILESYSTEM_DECL`, 41 of them alone on a line
with the declaration they belonged to gone. About 250 across a Boost.Filesystem split build
counting the rewritten headers, along with `BOOST_ATOMIC_DECL`,
`BOOST_FILESYSTEM_NO_SANITIZE_MEMORY` and `BOOST_ATTRIBUTE_UNUSED`.

A function's libclang extent begins at its return type. A macro invocation before that is
outside the extent, so removing the definition leaves the macro's source text in the
surrounding gap, which is copied verbatim.

## Why it is harmless

**Only macros that expand to nothing are ever left behind.** Anything that expands to a real
token produces a token for the cursor to cover, so the extent starts at the macro and it is
removed and reproduced with the declaration like any other specifier.

Measured across every expansion shape:

```cpp
#define M_NOTHING
#define M_ATTR    __attribute__((visibility("default")))
#define M_INLINE  inline
#define M_BOTH    inline __attribute__((always_inline))
#define M_COMMENT /* nothing but a comment */
```

| macro expands to | in the generated preamble |
|---|---|
| nothing | `M_NOTHING` left stray, then `int a(int v);` |
| an attribute | `M_ATTR    int b(int v);` — absorbed |
| `inline` | `M_INLINE  int c(int v);` — absorbed |
| both | `M_BOTH    int d(int v);` — absorbed |
| a comment | `M_COMMENT` left stray, then `int e(int v);` |

The two that are left behind expand to nothing and vanish at preprocessing. The three that
carry meaning are all handled correctly, including reproducing the specifier on the
declaration the preamble keeps in the definition's place.

So the leftovers are exactly the case where the text has no effect. There is no configuration
in which one of them becomes meaningful: if `BOOST_FILESYSTEM_DECL` were defined as
`BOOST_SYMBOL_EXPORT` for a dynamic build, it would expand to an attribute and would
therefore be absorbed rather than stranded.

### The correction

The previous revision argued that a dynamic build would strand visibility attributes, which
would then attach to the following declaration and silently export it. The experiment
supporting that -- a dangling `__attribute__((visibility("default")))` does change the
following function from `HIDDEN` to `DEFAULT` under `-fvisibility=hidden` -- was correct in
isolation but tested a situation the splitter cannot produce, because it never strands a
macro that expands to anything. The conclusion did not follow from the evidence, and the
severity was wrong by several levels.

## Why it is still worth fixing

Readability of generated output, which is the only thing anyone reads when a split goes
wrong. Two false leads in this project have already come from noise in generated files: this
one, and a `// Note: template - kept in preamble header` comment that was emitted for all ten
keep reasons, which sent someone looking for a template that was not there.

250 lines of dangling macro names in files people read while debugging is a small tax on every
future investigation.

## Implementation spec

When copying the gap before a removed definition, drop a trailing run of whitespace and whole
identifier tokens -- the specifiers libclang did not include because they expanded to nothing.
Stop at anything that cannot be part of a declaration specifier: `; } { ) : ,`, the start of
the file, or a line beginning with `#`.

Because only empty expansions reach this path, the transformation cannot change meaning; it
removes text the preprocessor was going to remove anyway. That makes it much safer than the
previous revision's version of the same change, and it does not need to handle attributes,
`inline`, or anything else that carries meaning.

Scan `blank_code_noise()`'s output and apply offsets to the original, as the rest of the file
does, so comments and string literals are not misread.

Two boundaries to respect:

- The adjusted start must be computed when the ranges are built, before the deduplication of
  identical `(start, end)` pairs that handles macros like `BOOST_BITMASK` expanding to many
  functions at one extent.
- The scan must not cross a preprocessor line, or an `#endif` could be swallowed and the
  conditional left unbalanced.

## Acceptance Criteria

- No line consisting only of an identifier appears in a generated preamble or rewritten
  header for the Boost example: about 250 to zero.
- Specifiers that expand to something are still reproduced on the declarations the preamble
  keeps, which the table above is the test for.
- The example still splits 12 of 12 translation units with no fallbacks, the library still
  links and passes its nine assertions, and the eleven tests still pass.
- Since the change is cosmetic, the generated objects should be unchanged: comparing the
  split library before and after is the cheapest way to confirm nothing meaningful moved.
