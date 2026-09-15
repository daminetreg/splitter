#include <cstdio>
#include "ops.h"
int use_alpha(int v);
int use_beta(int v);
int main()
{
    std::printf("%d %d %d\n", use_alpha(2) * OPS_SCALE, use_beta(2), via_gamma(2));
    return 0;
}
