// A virtual member defined under a macro the file undefines afterwards: flex's
// P4Lexer::yylex is written as `YY_DECL { ... }` and p4lexer.cc undefines YY_DECL at the
// end. It cannot be re-expanded in the definitions header, where the macro is gone, and it
// cannot stay in the preamble as `inline`: main.cpp was compiled against a non-inline key
// function and expects this unit to emit the vtable. The definitions piece compiles a
// variant of this unit with the definition in place (TODO/47).
#include "lexer.h"

#define LEX_DECL int Lexer::lex(int c)
LEX_DECL { return base + c; }
#undef LEX_DECL

int shifted(int v) { return v + 1; }
