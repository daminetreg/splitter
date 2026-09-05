// Regression fixture for TODO/10: a definition left in the preamble is compiled into every
// split piece, which is fine only if it may legally appear many times.
//
// `Base::~Base()` and `Base::kind()` are kept in the preamble because they are virtual --
// moving a virtual out of line means stripping an `override` that is usually spelled as a
// macro. But they are non-inline with external linkage, so every piece that included the
// preamble emitted them and `ld -r` rejected the object with `multiple definition`. They now
// go to a definitions header included by exactly one piece.
//
// Three further functions are needed: the collision only appears once more than one piece
// includes the preamble.
#include <iostream>

namespace demo {

struct Base {
    virtual ~Base();
    virtual int kind() const;
};

Base::~Base() {}
int Base::kind() const { return 7; }

int one() { return 1; }
int two() { return 2; }
int three() { return 3; }

}  // namespace demo

int main() {
    demo::Base b;
    int n = b.kind() + demo::one() + demo::two() + demo::three();
    std::cout << n << "\n";
    return n == 13 ? 0 : 1;
}
