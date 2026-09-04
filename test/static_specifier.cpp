// Regression fixture for TODO/02: removing the `static` declaration specifier from a
// split-out function must not touch `static_cast`, string literals, comments, or local
// static variables. Split this file and compare the emitted bodies against the source.
#include <cstddef>
#include <cstring>
#include <iostream>

namespace {

// Internal linkage comes from the anonymous namespace, so libclang reports this as
// static even though there is no `static` keyword to remove. The `static_cast` below
// used to be rewritten to `_cast`. Deliberately not `inline`: an inline definition is
// split out without a symbol being emitted, which is a separate defect (see TODO/07).
const char* find_sep(const char* p, std::size_t n) {
    return static_cast<const char*>(std::memchr(p, '/', n));
}

}  // namespace

// A real namespace-scope `static`: here the keyword must be removed from the split copy,
// while the `static_cast` in the body must survive.
static int scaled(int v) {
    return static_cast<int>(v) * 2;
}

// The word static appears below in a string literal and in a local static variable, and
// in this comment. None of them may be modified.
static const char* describe() {
    static const char* cached = "static storage";
    return cached;
}

int main() {
    const char* s = find_sep("a/b", 3);
    int n = (s ? 1 : 0) + scaled(3) + static_cast<int>(std::strlen(describe()));
    std::cout << n << "\n";
    return n == 21 ? 0 : 1;
}
