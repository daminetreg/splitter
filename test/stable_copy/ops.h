#pragma once
// Two inline functions; each unit of the fixture calls one of them. The unit that does not
// call alpha() gets a copy of this header that declares alpha() and does not define it, so
// an edit to alpha()'s body leaves that copy, its PCH and its pieces alone (TODO/48).
inline int alpha(int v) { return v * 10; }
inline int beta(int v) { return v + 1; }
// Named by a template, so it cannot be declared only: every unit's copy declares it and a
// piece of its own defines it, and an edit to its body recompiles that piece, not the PCH
// and the other pieces (TODO/49).
inline int gamma(int v) { return v - 1; }
template <class T> int via_gamma(T v) { return gamma(v); }
// Named by a template too, but its body calls a template declared here and defined nowhere
// the unit reads: a piece forced into existence would reference an instantiation nothing
// defines, so it keeps its body in the copy, as the plain build compiles it only if
// something uses it (TODO/49).
template <class T> int needs_definition(T v);
inline int delta(int v) { return needs_definition(v); }
template <class T> int via_delta(T v) { return delta(v); }
// A body that holds a directive is kept as written, whoever calls it: Boost's
// current_function.hpp defines BOOST_CURRENT_FUNCTION inside a helper nothing calls.
inline void helper()
{
#define OPS_SCALE 3
}
