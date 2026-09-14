// Deduced return types in every spelling: `auto`, `auto &`, `const auto &`, `auto *`. A
// function with one cannot be called before its definition is seen, so it stays in the
// preamble whatever surrounds the `auto`. p4c's IR headers write `inline auto &getExpr()`
// and `inline const auto &getExpr() const`, and the split left a declaration behind that
// every caller tripped over (TODO/47).
#include <cstdio>

struct Box {
    int value = 20;
    int *slot = &value;
    auto &ref() { return value; }
    const auto &cref() const { return value; }
    auto *ptr() { return slot; }
    auto plain() const { return value + 1; }
    int use() const { return cref() + plain(); }
};

int use_box()
{
    Box b;
    b.ref() += 1;
    return b.use() + *b.ptr();
}

int main()
{
    std::printf("%d\n", use_box());
    return use_box() == 64 ? 0 : 1;
}
