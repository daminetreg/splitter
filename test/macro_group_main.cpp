// Regression fixture for TODO/42 (1c): a macro invocation that expands to several
// external-linkage definitions, in a header. The splitter folds definitions that share one
// extent into one kept range and drops the FunctionInfo from it, so the range never reached
// the rule that moves an external-linkage kept definition to the definitions header. The
// group stayed in the header copy, every piece that included the preamble emitted all of its
// functions, and `ld -r` reported them multiply defined. On OpenCV: arithm.simd.hpp's
// recip*, accum.simd.hpp's accW_64f and accProd_64f.
#include "macro_group_defs.hpp"
#include <cstdio>

int consumer_one(int v) { return lib::kernels::add32s(v, 1) + lib::kernels::helper_twice(v); }
int consumer_two(int v) { return lib::kernels::sub16u(v, 1); }

int main()
{
    if (consumer_one(5) != 16) return 1;
    if (consumer_two(5) != 4) return 2;
    if (lib::kernels::add8u(250, 10) != 4) return 3;
    std::printf("%d %d %d\n", consumer_one(5), consumer_two(5), lib::kernels::add8u(250, 10));
    return 0;
}
