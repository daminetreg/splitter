// Covers splitting a header that mixes an always-inline function with an ordinary inline
// one that calls it.
//
// Clang gives an always-inline function available_externally linkage: it emits no
// out-of-line body and expects every call to be inlined, so nothing can link against one.
// Splitting turns both functions into separate translation units, and the piece holding
// `total` ends up with a real out-of-line call to `weigh` rather than an inlined copy.
//
// What keeps this working is that `weigh` is itself split out and forced into existence
// with __attribute__((used)), which does override available_externally. The test pins that
// down: drop the forcing, or stop stripping the attribute when re-emitting the definition,
// and `total` is left referring to a symbol that no object defines.
//
// Note what this does *not* cover: the case where the always-inline function stays in the
// header, is therefore never forced, and a split body calls it anyway. That is the shape
// behind the undefined references still seen when linking a program against a split
// Boost.Filesystem, and it has no reproduction here yet.
#pragma once

#define ALWAYS_INLINE inline __attribute__((__always_inline__))

namespace demo {

struct Weight {
    int grams;
};

ALWAYS_INLINE int weigh(Weight w) { return w.grams * 3; }

inline int total(Weight w) { return weigh(w) + 1; }

}  // namespace demo
