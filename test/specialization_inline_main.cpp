#include "specialization_inline_header.hpp"
#include <iostream>

double other_unit();

int main() {
    double n = rounded<double>(5.0) + rounded<float>(-2.0f) + scale<double>(3.0) + other_unit();
    std::cout << n << "\n";
    return n == 9.0 ? 0 : 1;   // 1 + (-1) + 6 + (-1 + 4)
}
