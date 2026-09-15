// A `#define` continued with backslashes between the unit's includes and the code: the include
// prefix precompiled for the parse used to be cut in the middle of it, leaving the macro
// empty and its continuation lines to be read as real namespaces opened around the rest of
// the unit. Boost.Filesystem's utf8_codecvt_facet.cpp defines BOOST_UTF8_BEGIN_NAMESPACE
// exactly so.
#include <cstdio>

#define OPEN_NS \
    namespace outer { \
    namespace inner {

#define CLOSE_NS \
    } \
    }

#include "continued_define_impl.ipp"

int main()
{
    const int n = outer::inner::Thing(4).twice();
    std::printf("%d\n", n);
    return n == 8 ? 0 : 1;
}
