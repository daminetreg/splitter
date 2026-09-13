// A header written to be included twice under two macro states, with a *section* guard at
// its end -- the shape of OpenCV's arithm.simd.hpp. It has no include guard: the section
// guard is not the file's first conditional, and a guard that is not is not a guard.
#ifdef PAIR_DECLARATIONS
int kernel(int v);
#endif

#ifdef PAIR_DEFINITIONS
int kernel(int v) { return v * 5; }     // external linkage, no inline
#endif

#ifndef PAIR_SECTION_GUARD
#define PAIR_SECTION_GUARD
#define PAIR_HELPER_MACRO(x) ((x) + 1)
#endif
