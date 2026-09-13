// A header written to be included twice under two macro states, with a *section* guard at
// its end -- the shape of OpenCV's arithm.simd.hpp. It has no include guard: the section
// guard is not the file's first conditional, and a guard that is not is not a guard.
#ifdef PAIR_DECLARATIONS
int kernel(int v);
int scaled(int v);
int shifted(int v);
#endif

#ifdef PAIR_DEFINITIONS
int kernel(int v) { return v * 5; }     // external linkage, no inline

// One macro invocation, one external function, its name spelled from the arguments as
// OpenCV's DEFINE_SIMD_U8(or, op_or) spells or8u: no declarator in the source covers it.
// PAIR_BODY is redefined between the two invocations, as arithm.simd.hpp redefines
// DEFINE_SIMD_FUN section by section: each definition means what its section's macro
// said, and re-expanded behind the whole header it would not.
#define PAIR_CAT_(a, b) a##b
#define PAIR_CAT(a, b) PAIR_CAT_(a, b)
#define PAIR_DEFINE(head, tail, k) int PAIR_CAT(head, tail)(int v) { return PAIR_BODY(v) * k; }

#define PAIR_BODY(v) ((v) + 1)
PAIR_DEFINE(sc, aled, 3)
#undef PAIR_BODY
#define PAIR_BODY(v) ((v) - 1)
PAIR_DEFINE(shif, ted, 2)
#endif

#ifndef PAIR_SECTION_GUARD
#define PAIR_SECTION_GUARD
#define PAIR_HELPER_MACRO(x) ((x) + 1)
#endif
