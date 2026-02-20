// Function: double average(const std::vector<double> &)
// Source: /home/runner/workspace/test/mylib.h (lines 19-23)
// ---

#include "mylib.h"

#line 19 "/home/runner/workspace/test/mylib.h"
inline double average(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}
