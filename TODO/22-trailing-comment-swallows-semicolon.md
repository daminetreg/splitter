# 22 — A trailing `//` comment on the declarator swallows the semicolon

**Severity:** Low, but cheap and unambiguous. 1 of the 62 fallbacks in the wider Boost run
(`chrono/src/process_cpu_clocks.cpp`).

## Motivation

When a definition moves out of a header, a declaration takes its place: the definition's text
up to the opening brace, plus `;`. If that text ends inside a `//` comment, the semicolon
lands inside the comment and the declaration has no terminator.

Boost.Chrono writes:

```cpp
  inline nanoseconds::rep tick_factor()        // multiplier to convert ticks
                            //  to nanoseconds; -1 if unknown
  {
    ...
  }
```

and the splitter leaves behind:

```cpp
nanoseconds::rep tick_factor()        // multiplier to convert ticks
                            //  to nanoseconds; -1 if unknown;
```

```
process_cpu_clocks.hpp:30:1: error: expected function body after function declarator
}
^
```

## Reproduction

```cpp
// d.hpp
#pragma once
namespace demo {
inline int tick_factor()      // multiplier
                              // -1 if unknown
{
    return 3;
}
}
```

```cpp
// d.cpp
#include "d.hpp"
int main() { return demo::tick_factor() == 3 ? 0 : 1; }
```

```
$ cpp-splitter clang++ -I. -c -o d.o d.cpp
d.o.split/include/d.hpp:7:1: error: expected function body after function declarator
d.o.split/d_preamble.h:5:1: error: expected '}'
[cpp-splitter] split build failed, falling back to normal compilation
```

Two comment lines are needed, not one: with a single trailing comment the `{` is on the same
line as the end of the comment often enough that `definition_decl_end()` cuts after it.

## Description

Two sites append the semicolon, both in `generate_preamble()`'s neighbourhood:

* `generate_forward_decl_inplace()` (`src/main.cpp:709`) ends with `return sig + ";";`, where
  `sig = trim_ws(fn.body.substr(0, decl_end))`.
* the member case, `preamble += apply_static_renames(r.fn->member_decl, renames) + ";"`.

`definition_decl_end()` returns the offset of the body's `{`, computed on
`blank_code_noise()` output, so comments are correctly ignored when *finding* the brace. What
is not ignored is that the text handed back still contains them, and `trim_ws` removes
whitespace, not a comment that runs to the end of the string.

The declaration is otherwise correct -- this is purely about where the `;` is written.

## Implementation spec

1. **One helper, both callers.** Add

   ```cpp
   // Terminate a declaration built from source text. The text may end inside a `//`
   // comment, in which case the semicolon has to start a new line or it is commented out.
   static std::string terminate_declaration(const std::string& decl);
   ```

   It blanks the text with `blank_code_noise()`, takes the last line of the blanked copy,
   and compares it against the same line of the original: if they differ, the line ends
   inside a comment and the result is `decl + "\n;"`. Otherwise `decl + ";"`.

2. **Use it at both sites.** Nothing else appends a semicolon to source-derived text; a grep
   for `+ ";"` should come back with exactly those two after the change.

3. **Leave the comments in.** Stripping them would be a second, larger change -- the comment
   is documentation the reader of the rewritten header benefits from, and it is what the
   original said. Only the terminator moves.

A `/* ... */` comment that is closed needs no special treatment: the blanked and original
last lines then agree from the closing `*/` onward, and an unclosed one cannot occur in text
that libclang parsed successfully.

## Acceptance Criteria

- The reproduction above splits without falling back, and the program returns 0.
- A regression fixture in `test/`: a header function whose declarator is followed by two
  `//` comment lines before the body, and a second one whose declarator ends in a closed
  `/* */` comment, to show the helper does not over-trigger.
- `chrono/src/process_cpu_clocks.cpp` no longer falls back.
