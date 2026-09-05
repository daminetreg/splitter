#include "macro_definition_driver.hpp"
#include <iostream>

int main() {
    collector::instance().mark(4);
    int n = collector::instance().v + driver_tag();
    std::cout << n << "\n";
    return n == 5 ? 0 : 1;
}
