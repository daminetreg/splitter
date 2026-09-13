// A unit that includes pair_header.hpp twice, the second time producing an external-linkage
// definition. The header cannot be split -- one copy would have to serve both inclusions --
// and left as it is it reaches every piece through the preamble. The splitter has to say so
// and compile the unit whole, before writing a single piece.
#include <cstdio>
#define PAIR_DECLARATIONS
#include "pair_header.hpp"
#undef PAIR_DECLARATIONS
#define PAIR_DEFINITIONS
#include "pair_header.hpp"
#undef PAIR_DEFINITIONS

int twice_kernel(int v) { return kernel(v) * 2; }

int main() { std::printf("%d\n", PAIR_HELPER_MACRO(twice_kernel(3))); return twice_kernel(3) == 30 ? 0 : 1; }
