#pragma once
// Two inline functions; each unit of the fixture calls one of them. The unit that does not
// call alpha() gets a copy of this header that declares alpha() and does not define it, so
// an edit to alpha()'s body leaves that copy, its PCH and its pieces alone (TODO/48).
inline int alpha(int v) { return v * 10; }
inline int beta(int v) { return v + 1; }
// A body that holds a directive is kept as written, whoever calls it: Boost's
// current_function.hpp defines BOOST_CURRENT_FUNCTION inside a helper nothing calls.
inline void helper()
{
#define OPS_SCALE 3
}
