#pragma once
// Named nowhere in the rewrapper invocation. Reproxy's C++ input processor has to find it and
// upload it, which is the whole reason the action is labelled type=compile.
inline int shared_base() { return 40; }

// Called by exactly one of the two units. The other includes this header and never calls it,
// so it is not emitted there and stays in that unit's rewritten copy of the header.
//
// That asymmetry is the whole point. It is the shape of Boost.Spirit's `one body` row -- 194
// units include the edited header and exactly one emits the function -- and it is what an
// edit to this body has to exercise: the unit that emits it patches a piece, the unit that
// keeps it patches its copy of the header, and neither should need a parse.
inline int shared_only_b() { return 7; }
