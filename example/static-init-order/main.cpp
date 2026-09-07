#include "registry.hpp"
#include <iostream>

int helper_one();
int helper_two();
int helper_three();
int helper_four();

int main() {
    const int sum = helper_one() + helper_two() + helper_three() + helper_four();

    // What the registrar recorded. If param_name was still empty when the registrar ran,
    // this is "" rather than "color_output" -- which is what Boost.Test reports as
    // "There is no argument provided for parameter color_output".
    std::cout << "registered: '" << (registry().names.empty() ? "<none>" : registry().names[0])
              << "'  sum=" << sum << "\n";

    // The *first* registration is the one that matters: a later, correct one does not undo a
    // parameter that was registered under an empty name.
    const bool registered_correctly =
        !registry().names.empty() && registry().names[0] == "color_output";
    return (registered_correctly && sum == 10) ? 0 : 1;
}
