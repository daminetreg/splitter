// Covers a declarator followed by a trailing `//` comment before the body.
//
// When a definition moves out of a header a declaration takes its place: the text up to the
// opening brace, plus a semicolon. If that text ends inside a `//` comment the semicolon
// lands inside the comment and the declaration has no terminator:
//
//     int tick_factor()      // multiplier
//                            // -1 if unknown;
//
//     error: expected function body after function declarator
//
// Two comment lines are needed to show it. With one, the brace is often on the same line as
// the end of the comment and the cut falls after it.
//
// `closed_comment` is the other half of the test: a declarator ending in a *closed* `/* */`
// comment is not inside a comment at the cut, so the semicolon belongs where it always was.
#pragma once

namespace demo {

inline int tick_factor()      // multiplier to convert ticks
                              // to nanoseconds; -1 if unknown
{
    return 3;
}

inline int closed_comment(int n) /* scale it */
{
    return n * tick_factor();
}

}  // namespace demo
