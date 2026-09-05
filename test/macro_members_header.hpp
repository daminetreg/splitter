// Covers a class whose members are produced by a macro expansion.
//
// Every declaration a macro produces reports the same source extent: the invocation, not
// the declarator, because that is where the tokens entered the file. Nothing useful can be
// done with such text. It cannot be edited -- the always-inline attribute libclang finds on
// TAGGED_ACCESSORS' members has the whole invocation as its extent, so cutting the
// attribute out cuts out the class members with it -- and it cannot be removed, because
// removing it removes every declaration the macro produced, not just the one being split.
//
// Before this was handled, splitting this header left `struct Tagged { inline };` behind: a
// bare declaration specifier inside a class body, which stops the header compiling and
// takes down every translation unit that includes it.
#pragma once

#define ALWAYS_INLINE inline __attribute__((__always_inline__))

#define TAGGED_ACCESSORS(N)                       \
    typedef int value_type;                       \
    ALWAYS_INLINE int scaled() const { return value * (N); }  \
    ALWAYS_INLINE int shifted() const { return value + (N); }

namespace demo {

struct Tagged {
    int value;
    TAGGED_ACCESSORS(4)
};

inline int combine(Tagged t) { return t.scaled() + t.shifted(); }

}  // namespace demo
