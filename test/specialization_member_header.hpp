// Covers a member function defined inside a full explicit specialization of a class
// template.
//
// A member of a class *template* is kept in the header: an out-of-line definition would have
// to repeat the template header, which a split .cpp cannot do. A full specialization is a
// different cursor kind and does not match that rule, so its members were split -- and then
// qualified with the name libclang reports for the parent, which is the template's name with
// the arguments dropped:
//
//     inline auto clz_dispatch::call(unsigned long n) -> unsigned long { ... }
//
//     error: 'clz_dispatch' is not a class, namespace, or enumeration
//
// The definition really is movable: unlike a class template, a full specialization takes an
// out-of-line member definition with no template<> prefix. Only the name was wrong.
//
// `Box` is the other half: a member of the primary template, which must still be kept, and a
// use that instantiates it so both paths are exercised in one program.
#pragma once

template <class U> struct clz_dispatch;

template <>
struct clz_dispatch<unsigned long> {
    static unsigned long call(unsigned long n) { return n + 1; }
};

template <>
struct clz_dispatch<unsigned int> {
    static unsigned int call(unsigned int n) { return n + 2; }
};

template <class T>
struct Box {
    T value;
    T doubled() const { return value + value; }
};
