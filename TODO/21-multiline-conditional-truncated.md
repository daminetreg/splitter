# 21 — A `#if` condition spread over several lines is replayed truncated

**Severity:** Medium. 2 of the 62 fallbacks in the wider Boost run
(`core/test/alloc_construct_test.cpp` and `core/test/pointer_traits_rebind_sfinae_test.cpp`).

## Motivation

`active_conditionals()` reads the source a line at a time and stores the text of each `#if`
it is inside. A condition continued with a backslash occupies more than one line, and only
the first is kept. The split piece then opens with a condition that ends in a line splice,
which swallows whatever the writer put next -- the `#line` directive:

```cpp
#include "alloc_construct_test_preamble.h"

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && \
#line 66 "alloc_construct_test.cpp"
void test_construct_args()
```

```
alloc_construct_test.cpp_7_test_construct_args.cpp:7:51:
    error: invalid token at start of a preprocessor expression
```

## Reproduction

```cpp
// c.cpp
#include <cstdio>
#define HAVE_A
#define HAVE_B
#if defined(HAVE_A) && \
    defined(HAVE_B)
int both() { return 7; }
#endif
int main() { std::printf("%d\n", both()); return 0; }
```

```
$ cpp-splitter clang++ -c -o c.o c.cpp
c.o.split/c.cpp_1_both.cpp:7:24: error: invalid token at start of a preprocessor expression
[cpp-splitter] split build failed, falling back to normal compilation
```

## Description

`active_conditionals()` (`src/main.cpp:1132`) is a hand-rolled line scanner:

```cpp
while (std::getline(iss, line)) {
    ...
    std::string t = trim_ws(line);
    if (t.empty() || t[0] != '#') { pending_guard.clear(); continue; }
    ...
    stack.push_back(t);
}
```

`t` is one physical line. Nothing joins continuations, so a multi-line condition is stored
half-written, and `emit_split_files()` writes it out as-is.

Three things follow from the same omission, and a fix should cover all of them:

* A continued `#if` is truncated, as above.
* A continued `#endif` or `#else` -- rare, but legal -- would not be recognised as closing
  anything, leaving the conditional stack unbalanced for every definition after it.
* A directive whose *name* is split across the splice (`#\` newline `if`) is not seen as a
  directive at all. Nothing in Boost does this; a lexer that handles splices gets it for
  free, a special case for `#if` does not.

The same scanner shape appears in `undefined_macros()`, which looks for `#undef NAME`. A
continued `#undef` is not a thing anyone writes, but the two functions should share whatever
line reader is introduced rather than diverge.

## Implementation spec

1. **Splice before scanning.** Add a helper that returns the source's *logical* lines: a
   physical line whose last non-whitespace character is `\` is joined with the next, with the
   backslash and the newline removed and a single space put in their place, so tokens on
   either side stay separate. Count how many physical lines were consumed so the running
   offset stays exact -- `active_conditionals()` compares that offset against the function's
   `start_offset`, and getting it wrong changes which conditionals a definition is reported to
   be inside.

2. **Feed both scanners from it.** `active_conditionals()` and `undefined_macros()` both take
   logical lines. Neither needs any other change: the stored `#if` text is then complete and
   valid on one line.

3. **Do not re-wrap.** Emitting the joined condition as a single long line in the piece is
   correct and simpler to reason about than reproducing the original line structure. The
   `#line` directive that follows it restores the numbering for diagnostics anyway.

4. **Comments.** A `/* ... */` comment inside a continued condition survives the join, since
   the join preserves everything but the splice. A `//` comment cannot appear before a splice
   in a valid directive, so no special handling is needed -- but the fixture should carry a
   `/* */` in the middle of a continued condition to pin that down.

## Acceptance Criteria

- The reproduction above splits without falling back, and the program prints `7`.
- A regression fixture in `test/`: a definition guarded by a condition continued over three
  physical lines, one of them carrying a `/* */` comment, plus a second definition after the
  matching `#endif` to prove the conditional stack stayed balanced.
- The two Boost translation units named above no longer fall back.
- The conditionals written into a split piece are byte-for-byte a valid preprocessor
  directive: no piece contains a line ending in `\` followed by `#line`.
