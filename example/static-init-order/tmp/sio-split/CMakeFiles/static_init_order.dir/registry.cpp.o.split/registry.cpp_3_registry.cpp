// Function: Registry & registry()
// Source: /home/daminetreg/workspace/cpp-splitter/example/static-init-order/registry.cpp (lines 13-16)
// ---

#include "registry_preamble.h"

#line 13 "/home/daminetreg/workspace/cpp-splitter/example/static-init-order/registry.cpp"
Registry& registry() {
    static Registry the_one;
    return the_one;
}
