// A definition that inherits its exception specification from a declaration in a system
// header. The compiler accepts the definition without the specification only because the
// prior declaration is in a system header; a forward declaration the splitter writes into
// the preamble is not, and the piece's definition then fails against it.
#include <cstdio>
#include "system_decl.hpp"

int compute(int v) { return v * 2; }

int main()
{
    std::printf("%d\n", compute(21));
    return compute(21) == 42 ? 0 : 1;
}
