#include "spec_member.hpp"
#include <iostream>

int main() {
    Box<int> b{6};
    int n = b.scaled() + b.weigh();
    std::cout << n << "\n";
    return n == 28 ? 0 : 1;
}
