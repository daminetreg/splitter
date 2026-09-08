#include <pkg/quoted_split.hpp>
#include <pkg/quoted_pair.hpp>
#include <cstdio>

int main() {
    std::printf("%d %d\n", quoted_split_value(), quoted_pair_value());
    return 0;
}
