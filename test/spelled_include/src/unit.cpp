// Includes are spelled from the tree's root, as p4c spells them (`lib/cstring.h`), while the
// unit sits in src/: the copy of a split header has to be where the spelling finds it.
#include <cstdio>
#include "src/wrapper.h"
#include "src/api.h"

int compute(int v) { Wrapper w{scale(v)}; return shift(w.scaled); }

int main()
{
    std::printf("%d\n", compute(5));
    return compute(5) == 19 ? 0 : 1;
}
