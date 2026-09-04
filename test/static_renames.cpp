// Regression fixture for TODO/03: renaming internal-linkage functions must also rewrite
// the text carried verbatim into the preamble, and must leave look-alike identifiers,
// string literals and comments alone.
#include <cstring>
#include <iostream>

static int scan(int v) { return v + 1; }

// Shares a prefix with `scan`: only the exact identifier may be rewritten.
static int scan_all(int v) { return scan(v) * 2; }

using scan_fn = int (*)(int);

// A namespace-scope pointer holding the address of a split-out static function. This is
// the case that used to fail with "use of undeclared identifier".
static scan_fn active_scan = &scan;

// The word scan appears in this comment and in the string literal below; neither may
// be touched by the rename.
static const char* label = "scan";

// Templates stay in the preamble, and this one calls a function that was split out.
template <typename T>
static T twice(T v) { return v * 2 + scan(0) - 1; }

int main() {
    int n = active_scan(1) + scan_all(3) + twice(5)
            + static_cast<int>(std::strlen(label));
    std::cout << n << "\n";
    return n == 24 ? 0 : 1;
}
