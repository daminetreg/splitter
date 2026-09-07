// Covers where a variable that may exist in only one object is put.
//
// Before C++17 there is nowhere good: it moves to the definitions header, which one piece
// includes, and its dynamic initialiser then runs in link order relative to anything that
// reads it during static initialisation. From C++17 it stays exactly where it is, marked
// `inline`, so the copies merge and the order relative to its neighbours is the order the
// source had.
//
// This fixture is compiled at C++17 by the test driver, so it asserts the good case: the
// registrar, which runs during static initialisation and reads `param_name`, must see the
// initialised value. example/static-init-order/ is the same shape built both ways.
#include <iostream>
#include <string>
#include <vector>

std::vector<std::string> seen;

// External linkage, not inline, dynamic initialiser: the shape that gets moved.
std::string param_name = "color_output";

struct Registrar {
    Registrar() { seen.push_back(param_name); }
};

static Registrar s_registrar;

int one()   { return 1; }
int two()   { return 2; }
int three() { return 3; }

int main() {
    const int sum = one() + two() + three();
    const std::string first = seen.empty() ? std::string("<none>") : seen[0];
    std::cout << first << " " << sum << "\n";
    return (first == "color_output" && sum == 6) ? 0 : 1;
}
