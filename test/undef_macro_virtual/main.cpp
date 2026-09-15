#include <cstdio>
#include "lexer.h"

int main()
{
    Lexer l;
    Lexer *p = &l;
    std::printf("%d %d\n", p->lex(5), shifted(1));
    return p->lex(5) == 15 && shifted(1) == 2 ? 0 : 1;
}
