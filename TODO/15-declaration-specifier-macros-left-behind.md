# 15 — A macro before a function's return type is left behind when the definition is removed

**Severity:** High, and currently latent. The Boost example builds statically, where
`BOOST_FILESYSTEM_DECL` expands to nothing and the stray text is invisible. Configure the
same tree for dynamic linking and those 250 leftovers become visibility attributes attached
to whatever declaration happens to follow them.

## Motivation

Generated preambles are full of this:

```cpp
//  normal  --------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL 


BOOST_FILESYSTEM_DECL 


//  generic_path ---------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL 
```

`path_preamble.h` contains 43 occurrences of `BOOST_FILESYSTEM_DECL`, of which **41 are
stray** -- the macro alone on a line, its declaration gone. Across a Boost.Filesystem split
build: 120 stray lines in 9 preambles, and another 130 in the rewritten headers, so about 250
in total. `BOOST_ATOMIC_DECL`, `BOOST_FILESYSTEM_NO_SANITIZE_MEMORY` and
`BOOST_ATTRIBUTE_UNUSED` are left behind the same way.

### Why it does not currently break anything

The example is built with `BUILD_SHARED_LIBS=OFF` and no `BOOST_ALL_DYN_LINK`, so
`boost/filesystem/config.hpp` takes this branch:

```cpp
#else
#define BOOST_FILESYSTEM_DECL
#endif
```

The macro expands to nothing and the stray text vanishes at preprocessing. That is the only
reason 250 dangling declaration specifiers have gone unnoticed.

### Why it will

With `BOOST_ALL_DYN_LINK` or `BOOST_FILESYSTEM_DYN_LINK` the same macro becomes
`BOOST_SYMBOL_EXPORT`, that is `__attribute__((visibility("default")))`. A dangling attribute
is not discarded: it attaches to the next declaration. Measured directly, compiling with
`-fvisibility=hidden`, which this build already passes:

```cpp
#define DECL __attribute__((visibility("default")))
DECL

DECL

int leaked() { return 1; }
```

| | visibility of `leaked()` |
|---|---|
| preceded by stray attributes | `GLOBAL DEFAULT` |
| control, nothing before it | `GLOBAL HIDDEN` |

So in a shared-library build the effect is that an arbitrary set of functions -- whichever
ones happen to follow a removed definition -- get exported when they should not be. That is a
silent ABI change, not a compile error, which is the worst shape for it to take.

Where nothing at all follows the stray macro before the end of a block, the same text is a
syntax error instead.

## Reproduction

```sh
./benchmark-boost-split.sh            # or any split build of the example
grep -c '^BOOST_FILESYSTEM_DECL[[:space:]]*$' \
    /tmp/bench-split/libs/filesystem/CMakeFiles/boost_filesystem.dir/src/path.cpp.o.split/path_preamble.h
```

For the semantic half, add `-DBOOST_ALL_DYN_LINK` to the configure line and compare
`readelf -sW` output for a function that follows a split-out definition against the same
function in an unsplit build.

## Description

`generate_preamble()` removes each function by its libclang extent, `[start_offset,
end_offset)`. For

```cpp
BOOST_FILESYSTEM_DECL path path_algorithms::lexically_normal_v3(path const& p)
{
    ...
}
```

the extent begins at `path`, the return type -- not at `BOOST_FILESYSTEM_DECL`. When the
macro expands to nothing there is no token for it to cover, so the cursor cannot start any
earlier, and the source text of the macro invocation sits outside the range that gets cut.
Everything from the previous item up to `start_offset` is copied verbatim into the preamble,
and that includes the macro.

This is not specific to attribute macros. Any macro invocation between the previous
declaration and the start of a function's extent survives the removal.

## Implementation spec

### The rule

When removing or replacing a function's extent, extend the start of the removed range
backwards over the declaration specifiers that libclang did not include: whitespace, and
whole identifier tokens separated only by whitespace. Stop at the first character that cannot
be part of a declaration specifier -- any of `; } { ) : ,` or the start of the file, or a
preprocessor directive line.

Identifiers are the only thing that needs absorbing. Real keywords like `inline`, `static`
and `constexpr` are already inside the extent, because they produce tokens; only macro
invocations that expanded to nothing, or to an attribute clang attributed to the declaration
rather than to the range, are left outside it.

### Where

`generate_preamble()`, on the `Range` built for each `FunctionInfo`. Compute the adjusted
start once when the ranges are built, so that both the gap-copying and the sorting see the
same boundaries; adjusting it later would let a gap and a range overlap.

Note that `Range` entries are deduplicated by identical `(start, end)` for the macro-expansion
case described in the comment there -- `BOOST_BITMASK` and friends -- so the widened start
must be computed before that comparison, not after.

### Boundary cases to get right

1. **Two definitions with nothing between them.** Widening the second must not run back into
   the first; stopping at `}` handles it.
2. **A definition at the start of a namespace body.** Stopping at `{` handles it.
3. **A macro invocation with arguments**, `BOOST_ATTR(x) void f() {}`. Stopping at `)` leaves
   the invocation behind, which is the status quo rather than a regression. Absorbing a
   balanced parenthesised group before an identifier would handle it, and is worth doing only
   if such a case actually appears.
4. **A preprocessor directive immediately before**, `#endif` then the definition. The scan
   must not cross a line beginning with `#`, or a conditional's `#endif` would be swallowed
   and the preprocessor left unbalanced.
5. **A comment before the definition.** `blank_code_noise()` already blanks comments, so
   scanning the blanked copy and applying offsets to the original keeps them intact, as the
   rest of the file does.
6. **The out-of-line member form.** `prepare_functions()` builds `member_decl` and
   `outlined_body` from `fn.body`, which starts at the same extent, so a specifier before the
   return type is missing from those too. The widened start should be recorded on
   `FunctionInfo` and used wherever `body` is taken, not only in the preamble.

## Acceptance Criteria

- No line consisting only of an all-capitals identifier appears in any generated preamble or
  rewritten header for the Boost example; the count goes from about 250 to zero.
- `BOOST_FILESYSTEM_DECL` still appears where it belongs: on the declarations the preamble
  legitimately keeps.
- Configured with `-DBOOST_ALL_DYN_LINK`, the set of exported symbols in the split library
  matches the set in an unsplit build, compared with `readelf -sW`. This is the criterion that
  matters; the others are proxies for it.
- The example still splits 12 of 12 translation units with no fallbacks, and the library still
  links and passes its nine assertions.
- A regression fixture in `test/`, registered with `add_test`, in which a function carrying a
  macro-expanded declaration specifier is split out, asserting the specifier does not survive
  in the preamble and does not attach to the following declaration.
