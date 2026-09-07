// Function: int main()
// Source: /home/daminetreg/workspace/cpp-splitter/example/static-init-order/main.cpp (lines 9-29)
// Note: kept in the preamble, not compiled -- the program's entry point
// ---

#include "main_preamble.h"

#line 9 "/home/daminetreg/workspace/cpp-splitter/example/static-init-order/main.cpp"
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

    size_t i=0;
    for (const auto& name : registry().names) {
      std::cout << "registry().names[" << i << "] = \"" << name << "\"\n"; 
      ++i;
    }
    return (registered_correctly && sum == 10) ? 0 : 1;
}
