#include "specialization_member_header.hpp"
#include <iostream>

int main() {
    unsigned long a = clz_dispatch<unsigned long>::call(1);   // 2
    unsigned int  b = clz_dispatch<unsigned int>::call(1);    // 3
    Box<int> box{5};
    int n = static_cast<int>(a) + static_cast<int>(b) + box.doubled();
    std::cout << n << "\n";
    return n == 15 ? 0 : 1;
}
