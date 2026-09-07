// Covers a `static` variable at namespace scope in the translation unit's own source.
//
// It has internal linkage, so the language says one copy per translation unit. The splitter
// turns one translation unit into many pieces, each including the preamble and becoming an
// object of its own -- so a `static` left in the preamble is one copy per *piece*, and all of
// them end up in the single object the build system asked for. Anything that counts, caches
// or registers then behaves differently: this counter would reach 1 instead of 3, because
// each increment lands in a different copy.
//
// It is renamed and moved to the definitions header, exactly as a `static` function in a .cpp
// already is. `same_name` is the reason the rename is needed rather than just the move: the
// other translation unit defines a `static` variable spelled identically, and after both lose
// their internal linkage the two must still not collide.
#include <iostream>

static int calls = 0;
static int same_name = 7;

int bump_once()  { ++calls; return calls; }
int bump_twice() { ++calls; ++calls; return calls; }
int local_value() { return same_name; }

int other_unit_value();

int main() {
    bump_once();
    bump_twice();

    const int n = calls;
    const int mine = local_value();
    const int theirs = other_unit_value();

    std::cout << n << " " << mine << " " << theirs << "\n";
    return (n == 3 && mine == 7 && theirs == 11) ? 0 : 1;
}
