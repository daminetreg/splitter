// A static of a type with no linkage that is safe to keep in the preamble: a captureless
// lambda, as Boost.Spirit's x3 tests write `static auto fun1 = [](auto& ctx) {...};`. Its
// closure type cannot be named from another translation unit, but it holds nothing, so the
// copy each piece gets is the same object for every purpose. This unit splits; it is the
// counterpart of split.static_unnamed_type_declines.
#include <cstdio>

static auto twice = [](int v) { return v * 2; };
static const auto shift = [](int v) { return v + 1; };

int use_one(int v) { return twice(v); }
int use_two(int v) { return shift(twice(v)); }

int main()
{
    if (use_one(20) != 40 || use_two(20) != 41) return 1;
    std::printf("%d %d\n", use_one(20), use_two(20));
    return 0;
}
