#include "bench_common.hpp"

#include <cmath>

int main()
{
    double const total = measure_area()
                       + measure_distance()
                       + measure_overlay()
                       + measure_hull();

    std::cout.precision(6);
    std::cout << std::fixed << total << "\n";

    // A value rather than a bare exit status, so a split build that computes something
    // different is caught rather than merely linking.
    return std::isfinite(total) && total > 0.0 ? 0 : 1;
}
