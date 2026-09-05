#pragma once
#include <iostream>

inline int probe() { return 7; }

// An ordinary function that must still be split, so the fixture cannot pass by the whole
// header being left alone.
inline int helper() { std::cout << "helper\n"; return 3; }

// Declared here, defined by the translation unit, and called from the `main` the .ipp
// supplies -- which is how Boost.Test's header-only framework is wired.
int unit_contribution();

#include "main_in_header_impl.ipp"
