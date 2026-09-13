// Regression fixture for TODO/42 (1b): an implementation include at the *end* of a source
// file, after the unit's own definitions. libclang's clang_getInclusions() reported the
// includes at the top of the file -- those the prefix PCH covers -- and not this one, so the
// header was never a split candidate. Its definitions then stayed verbatim in the preamble
// and every piece of the unit emitted them: `multiple definition of Loader::~Loader()`.
#include "tail_include_decl.hpp"
#include <cstdio>

int helper_one() { return 1; }
int helper_two() { return 2; }

int main()
{
    Loader l;
    if (l.value() != 42) return 1;
    std::printf("%d %d %d\n", helper_one(), helper_two(), l.value());
    return 0;
}

#include "tail_include_impl.hpp"
