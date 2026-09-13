// Regression fixture for TODO/42 (1): function definitions inside an `extern "C"` linkage
// specification. OpenCV writes its C API this way through a macro:
//
//     CV_IMPL void cvMaxS(const void* src, double value, void* dst) { ... }
//
// with CV_IMPL expanding to `extern "C"`. libclang reports such a definition under a
// CXCursor_LinkageSpec, and a harvest that does not descend into one never sees it: the
// definition stays in the preamble verbatim, every header piece that includes the preamble
// emits it, and `ld -r` reports it multiply defined. On OpenCV that was 45 of 158 units.
#include "extern_c_api.hpp"
#include <cstdio>

#define MY_IMPL extern "C"

MY_IMPL int api_add(int a, int b)
{
    return helper_twice(a) + b;
}

MY_IMPL int
api_scale(int v)
{
    return helper_twice(v) * 10;
}

// The braced form, as a header of C declarations would use it.
extern "C" {
int api_negate(int v)
{
    return -helper_twice(v);
}
}

int main()
{
    if (api_add(3, 4) != 10) return 1;
    if (api_scale(2) != 40) return 2;
    if (api_negate(5) != -10) return 3;
    std::printf("%d %d %d\n", api_add(3, 4), api_scale(2), api_negate(5));
    return 0;
}
