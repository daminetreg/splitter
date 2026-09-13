// Regression fixture for TODO/42 (4): a function whose declarator differs by preprocessor
// branch, the body following the #endif. OpenCV's alloc.cpp:
//
//     #ifdef OPENCV_ALLOC_ENABLE_STATISTICS
//     static inline
//     void* fastMalloc_(size_t size)
//     #else
//     void* fastMalloc(size_t size)
//     #endif
//     { ... }
//
// The declaration left in place is the text up to the body plus a `;` -- and that `;` was
// appended to the `#endif` line, which is not a place a token can go.
#include <cstdio>
#include <cstdlib>

#ifdef ALLOC_WITH_STATISTICS
static inline
void* fast_malloc_(size_t size)
#else
void* fast_malloc(size_t size)
#endif
{
    void* p = std::malloc(size);
    return p;
}

void fast_free(void* p) { std::free(p); }

int main()
{
    int* p = static_cast<int*>(fast_malloc(sizeof(int)));
    if (!p) return 1;
    *p = 42;
    std::printf("%d\n", *p);
    fast_free(p);
    return 0;
}
