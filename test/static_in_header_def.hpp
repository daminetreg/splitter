// One half of the static-in-a-header fixture: a function with internal linkage, defined in a
// header, called from *another* header.
//
// A split-out definition with internal linkage has to become external, or no other piece can
// call it, and it has to be renamed, or objects from different translation units collide
// when `ld -r` merges them. That rename is built per split unit -- and a translation unit is
// split into many units, one per header. So this function was renamed in its own rewritten
// header and nowhere else, and static_in_header_use.hpp was left calling a name that no
// longer existed:
//
//     error: use of undeclared identifier 'scale'
//
// Two headers are essential. With both functions in one header the rename map covers both
// the definition and the use, and it works.
#pragma once

namespace demo {

static inline int scale(int n) { return n * 2; }

}  // namespace demo
