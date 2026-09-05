// Covers a definition guarded by a condition spread over several physical lines.
//
// A split piece replays the conditionals its definition was written under. The scanner that
// collects them read the source a physical line at a time, so a condition continued with a
// backslash was stored half-written -- and the trailing backslash then spliced whatever the
// piece emitted next, the `#line` directive, into the condition:
//
//     #if defined(HAVE_A) && \
//     #line 6 "multiline_conditional_main.cpp"
//     int both() { return 7; }
//
//     error: invalid token at start of a preprocessor expression
//
// `later()` is here to prove the conditional stack stayed balanced across the continued
// `#endif`: if the scanner loses track of the nesting, every definition after it is emitted
// under the wrong conditions.
#include <cstdio>

#define HAVE_A
#define HAVE_B

#if defined(HAVE_A) /* a comment inside a continued condition */ && \
    defined(HAVE_B) && \
    !defined(HAVE_C)
int both() { return 7; }
#endif

int later() { return both() + 1; }

int main() {
    int n = both() + later();
    std::printf("%d\n", n);
    return n == 15 ? 0 : 1;
}
