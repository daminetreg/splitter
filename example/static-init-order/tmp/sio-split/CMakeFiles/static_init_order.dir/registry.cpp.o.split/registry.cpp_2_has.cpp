// Function: bool Registry::has(const std::string &)
// Source: /home/daminetreg/workspace/cpp-splitter/example/static-init-order/registry.cpp (lines 7-11)
// ---

#include "registry_preamble.h"

#line 7 "/home/daminetreg/workspace/cpp-splitter/example/static-init-order/registry.cpp"
bool Registry::has(std::string const& n) const {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return true;
    return false;
}
