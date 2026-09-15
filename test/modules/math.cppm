// A module interface unit the launcher splits: the interface keeps the declarations and the
// inline body, each non-inline body becomes an implementation unit of `math`. TODO/43.
module;
#include <cstdio>
export module math;

export inline int twice(int v) { return v * 2; }   // inline: stays in the BMI
int helper_kept() { return 7; }                    // not exported: moved to a piece
export int seven() { return helper_kept(); }       // exported, non-inline: moved to a piece
export int plus_one(int v);                        // defined in the implementation unit
