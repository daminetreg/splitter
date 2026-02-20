// Function: int main()
// Source: test/use_mylib.cpp (lines 4-15)
// ---

#include "use_mylib_preamble.h"

#line 4 "/home/runner/workspace/test/use_mylib.cpp"
int main() {
    std::cout << "add(3, 4) = " << add(3, 4) << "\n";
    std::cout << "multiply(5, 6) = " << multiply(5, 6) << "\n";
    std::cout << greet("World") << "\n";

    std::vector<double> vals = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::cout << "average = " << average(vals) << "\n";

    std::cout << "max_of(10, 20) = " << max_of(10, 20) << "\n";

    return 0;
}
