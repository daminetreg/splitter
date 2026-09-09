#include <pkg/hv_constants.hpp>
#include <cstdio>
#include <cstring>

struct Row { int id; char const* name; };

// An array whose bound comes from its initialiser. Moved to the definitions header it would
// leave `extern Row rows[];` behind -- the right declaration for an array of unknown bound and
// the wrong one for the sizeof below, which then fails to compile. From C++17 it stays where
// it is, marked `inline`. TODO/33.
Row rows[] = { {1, "a"}, {2, "b"}, {3, "c"} };

int row_count()         { return (int)(sizeof(rows) / sizeof(rows[0])); }
int count_from_header() { return hv_count; }
int greeting_len()      { return (int)std::strlen(hv_greeting); }

int main() {
    std::printf("%d %d %d\n", row_count(), count_from_header(), greeting_len());
    return 0;
}
