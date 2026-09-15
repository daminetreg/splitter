#pragma once
struct Lexer {
    virtual ~Lexer() = default;
    virtual int lex(int c);   // the key function: its unit emits the vtable
    int base = 10;
};
int shifted(int v);
