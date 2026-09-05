// Covers a constructor whose definition arrives through an implementation include.
//
// A constructor is not one symbol but a family -- C1 complete-object, C2 base-object -- and
// only the compiler decides which of them to emit. Split out, the definition is written
// `inline` and pinned with __attribute__((used)), which forces exactly one variant into
// existence: the piece emits C2 while every caller, no longer able to inline it, asks for
// C1. The link then fails on a symbol `nm -C` reports as present, because both variants
// demangle to the same text.
//
// Constructors in headers are kept in the preamble for that reason. A .ipp had not counted
// as a header, so this one was split, and linking anything against a split
// Boost.Serialization failed on utf8_codecvt_facet's constructor.
#pragma once

struct Counter {
    int value;
    int* sink;

    Counter(int start, int* marks);
    ~Counter();

    int doubled() const { return value * 2; }
};

#include "ipp_ctor_impl.ipp"
