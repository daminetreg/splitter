#include "macro_argument_extent_header.hpp"
#include <iostream>

int main() {
    version_type a(2);
    a.assign(3u);
    unsigned int n = a.get();
    std::cout << n << "\n";
    return n == 3u ? 0 : 1;
}
