#include "inline_header.hpp"
#include <iostream>

int from_a();

int from_b() { return demo::weigh(1) + static_cast<int>(demo::span("de")); }

int main() {
    int n = from_a() + from_b();
    std::cout << n << "\n";
    return n == 16 ? 0 : 1;
}
