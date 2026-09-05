#include "static_in_header_use.hpp"
#include <iostream>

// A static function in the source itself is still renamed and split; only the header case
// changed. Keeping one here means the rename cannot be quietly disabled everywhere.
static int local_triple(int n) { return n * 3; }

int main() {
    int n = demo::scaled_plus(3) + local_triple(2);
    std::cout << n << "\n";
    return n == 13 ? 0 : 1;
}
