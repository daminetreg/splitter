// Regression fixture for TODO/07: an inline function split out of a *header* keeps
// `inline`, because several translation units may each carry a copy, and the symbol has
// to be forced into existence or nothing is emitted at all.
#pragma once
#include <cstddef>

namespace demo {

inline int weigh(int v) { return v * 3 + 1; }

inline std::size_t span(const char* s) {
    std::size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

}  // namespace demo
