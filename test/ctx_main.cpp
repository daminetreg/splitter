#include "ctx_base.hpp"
#include "ctx_user.hpp"
#include <iostream>

int main() {
    int n = demo::total(demo::Weight{5});
    std::cout << n << "\n";
    return n == 30 ? 0 : 1;
}
