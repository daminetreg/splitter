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

int main()
{
    std::printf("%d %d %d\n", PAIR_HELPER_MACRO(twice_kernel(3)), scaled(4), shifted(4));
    return twice_kernel(3) == 30 && scaled(4) == 15 && shifted(4) == 6 ? 0 : 1;
}
