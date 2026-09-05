#include "no_linkage_type_driver.hpp"
#include <iostream>

int contribution() { return 2; }

int main() {
    int n = run_helper() + nl_tag() + contribution();
    std::cout << n << "\n";
    return n == 8 ? 0 : 1;
}
