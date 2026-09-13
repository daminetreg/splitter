// A unit that includes pair_header.hpp twice, the second time producing external-linkage
// definitions. One rewritten copy cannot serve both inclusions, so each is split on its own
// (TODO/44 B): the second into a copy of its own, named on this unit's second #include line
// in the generated preamble, and its definitions compiled in the macro state of that
// inclusion, at the position they were written.
#include <cstdio>
#define PAIR_DECLARATIONS
#include "pair_header.hpp"
#undef PAIR_DECLARATIONS
#define PAIR_DEFINITIONS
#include "pair_header.hpp"
#undef PAIR_DEFINITIONS

int twice_kernel(int v) { return kernel(v) * 2; }

// A body fragment read twice: no definition in it, so no copy, and the preamble keeps
// reading the original on both lines. The class template stays in the preamble with its
// #include line, as OpenCV's connectedcomponents.cpp keeps its labelling functors.
#define PAIR_STEP(v) ((v) * 2 + 1)
template <class T>
struct Frag {
    static T a(T v)
    {
#include "pair_fragment.inc"
        return v;
    }
};
template <class T>
T frag_b(T v)
{
#include "pair_fragment.inc"
    return v + 1;
}
int frag_a(int v) { return Frag<int>::a(v); }

int main()
{
    std::printf("%d %d %d %d %d\n", PAIR_HELPER_MACRO(twice_kernel(3)), scaled(4), shifted(4),
                frag_a(1), frag_b<int>(1));
    return twice_kernel(3) == 30 && scaled(4) == 15 && shifted(4) == 6 && frag_a(1) == 3 &&
                   frag_b<int>(1) == 4
               ? 0
               : 1;
}
