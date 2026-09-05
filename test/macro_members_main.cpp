#include "macro_members_header.hpp"
#include <iostream>

int main() {
    demo::Tagged t{6};
    demo::Tagged::value_type n = demo::combine(t);
    std::cout << n << "\n";
    return n == 34 ? 0 : 1;
}
