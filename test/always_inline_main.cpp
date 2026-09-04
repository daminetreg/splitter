#include "always_inline_header.hpp"
#include <iostream>

int main() {
    int n = demo::total(demo::Weight{5});
    std::cout << n << "\n";
    return n == 16 ? 0 : 1;
}
