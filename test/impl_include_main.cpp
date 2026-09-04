// Every definition in this translation unit arrives through an implementation include, the
// same shape as libs/filesystem/src/utf8_codecvt_facet.cpp pulling in its .ipp. The unit has
// no functions of its own, so its preamble still has to be written for the pieces split out
// of the .ipp to include, and the reachability roots have to be found in the .ipp rather
// than in the file being compiled.
#include "impl_include.ipp"
#include <iostream>

int main() {
    int n = demo::one() + demo::two() + demo::three();
    std::cout << n << "\n";
    return n == 6 ? 0 : 1;
}
