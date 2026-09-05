#include "trailing_comment_header.hpp"
#include <iostream>

int main() {
    int n = demo::tick_factor() + demo::closed_comment(4);
    std::cout << n << "\n";
    return n == 15 ? 0 : 1;
}
