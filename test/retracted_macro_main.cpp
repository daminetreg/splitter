#include "retracted_macro_header.hpp"
#include <iostream>

// Taking the address defeats inlining, so the object really has to define both.
int main() {
    int (*d)(int) = &demo::fast_double;
    int (*t)(int) = &demo::triple;
    int n = d(7) + t(5);
    std::cout << n << "\n";
    return n == 29 ? 0 : 1;
}
