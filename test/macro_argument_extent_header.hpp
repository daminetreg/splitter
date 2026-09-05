// Covers two definitions from one macro whose extents overlap without being equal.
//
// Every definition a macro produces reports the invocation as its extent -- except that a
// macro argument is spelled in the invocation, not in the macro body. So a definition whose
// first token comes from an argument has its extent start mapped to where the argument was
// written, inside the invocation rather than at its beginning. `assign` is that definition:
// it begins with `D&`, and `D` is an argument. The constructor begins with the expansion of
// the macro body, so its extent starts at `ST`.
//
// One macro, two extents: [ST ... )] and [version_type ... )]. They overlap and compare
// unequal, so a collapse that matched only identical extents left both in the list, and the
// emit loop wrote the invocation and then re-emitted its tail:
//
//     ST(unsigned int, version_type)
//     version_type)
//
//     error: expected unqualified-id
//
// This is BOOST_STRONG_TYPEDEF in boost/serialization/serialization.hpp, which took 18
// translation units down with it. Give `assign` a return type that is not a macro argument
// and the two extents become identical, the old collapse works, and nothing reproduces.
#pragma once

#define ST(T, D)                                   \
struct D {                                         \
    T t;                                           \
    explicit D(const T& v) : t(v) {}               \
    D& assign(const T& r) { t = r; return *this; } \
    T get() const { return t; }                    \
};

ST(unsigned int, version_type)
