#include "incremental_header.hpp"
#include <cstdio>

int main() {
    Counter c;
    std::printf("%d %d\n", c.value(), c.twice());
    return 0;
}
