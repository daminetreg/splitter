// Compiled with -stdlib=libc++. The libclang parse has to read the same standard library
// the compiler does: probed without the flag, the include paths were libstdc++'s, the parse
// took the other branch below, and the piece it wrote defined probe() a second time. p4c is
// built with libc++ because clang 13 cannot compile libstdc++ 13's <chrono> in C++20
// (TODO/47).
#include <cstdio>
#include <string>

#if defined(_LIBCPP_VERSION)
int probe() { return 1; }
#else
int probe() { return 2; }
#endif

int main()
{
    std::printf("%d\n", probe());
    return probe() == 1 ? 0 : 1;
}
