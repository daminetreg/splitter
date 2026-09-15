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

// A constructor whose initialiser list is under a conditional that opens and closes inside
// the removed part: bison's `P4Parser::P4Parser(...) #if YYDEBUG : a(), #else : #endif b()`.
struct Parser {
    Parser(int seed);
    virtual int input(char *buf, int max_size);   // virtual: kept, moved to the definitions header
    int debug;
    int value;
};

// A virtual member -- kept whole, moved to the definitions header -- whose declarator is
// written across a conditional: flex's yyFlexLexer::LexerInput.
#ifdef INTERACTIVE
int Parser::input(char *buf, int /* max_size */)
#else
int Parser::input(char *buf, int max_size)
#endif
{
    buf[0] = 'x';
    return max_size + value;
}
Parser::Parser(int seed)
#ifdef WITH_DEBUG
    : debug(1),
#else
    : debug(0),
#endif
      value(seed * 3) {}

int main()
{
    Parser p(5);
    char buf[4];
    std::printf("%d %d %d %d %d\n", parse("123"), twice(parse("4")), p.debug, p.value,
                p.input(buf, 2));
    return parse("123") == 6 && twice(parse("4")) == 8 && p.debug == 0 && p.value == 15 &&
                   p.input(buf, 2) == 17
               ? 0
               : 1;
}
