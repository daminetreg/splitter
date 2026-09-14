// A declarator that differs by preprocessor branch, with the `{` inside each branch and the
// `#endif` after it, in the body: p4c's parseInput.cpp. The declaration left in the
// preamble ends at the `{`, so it has to close the conditional the removed body closed, or
// the `#ifdef` stays open and the preamble does not preprocess (TODO/47).
#include <cstdio>

#ifdef WITH_VERSION
int parse(const char *text, int version) {
#else
int parse(const char *text) {
#endif
    int n = 0;
    while (*text) n += *text++ - '0';
#ifdef WITH_VERSION
    n += version;
#endif
    return n;
}

int twice(int v) { return v * 2; }

int main()
{
    std::printf("%d %d\n", parse("123"), twice(parse("4")));
    return parse("123") == 6 && twice(parse("4")) == 8 ? 0 : 1;
}
