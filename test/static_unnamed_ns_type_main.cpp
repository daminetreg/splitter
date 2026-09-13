// Fixture for TODO/44 (A): a static variable of a class a header defines in an unnamed
// namespace, as OpenCV's alloc.cpp declares `static AllocatorStatistics allocator_stats;`.
// The type cannot be named from another translation unit, and every piece is one. Kept in
// the preamble it would be one object per piece; moved out, nothing could declare it. The
// splitter keeps the variable and every function that uses it together, in the one
// translation unit that may hold single instances, and splits the rest.
#include "static_unnamed_ns_type.hpp"
#include <cstdio>

static Counter stats;

void record(int v) { stats.note(v); }
int recorded_total() { return stats.total(); }
int recorded_count() { return stats.count(); }

// Untouched by the variable: split as usual.
int unrelated_twice(int v) { return v * 2; }
int unrelated_plus(int a, int b) { return a + b; }

int main()
{
    record(unrelated_twice(5));
    record(unrelated_plus(20, 12));
    if (recorded_total() != 42 || recorded_count() != 2) return 1;
    std::printf("%d %d\n", recorded_total(), recorded_count());
    return 0;
}
