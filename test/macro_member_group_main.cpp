// One macro invocation defining out-of-line members of several classes, as p4c's
// dbprint-expression.cpp defines IR::UPlus::dbprint and its siblings through
// ALL_UNARY_OPS(UNOP_DBPRINT). Members have external linkage and the class declares them,
// so the group moves whole to the definitions header with nothing left behind; kept in
// the preamble, every piece emitted all of them (TODO/47).
#include <cstdio>

struct Plus { int v; int apply() const; };
struct Neg { int v; int apply() const; };
struct Twice { int v; int apply() const; };

#define ALL_OPS(M) M(Plus, +v) M(Neg, -v) M(Twice, v * 2)
#define DEFINE_APPLY(NAME, EXPR) int NAME::apply() const { return EXPR; }
ALL_OPS(DEFINE_APPLY)

int sum() { return Plus{3}.apply() + Neg{4}.apply() + Twice{5}.apply(); }
// A second piece: with the group in the preamble, both pieces defined every member.
int product() { return Plus{3}.apply() * Twice{5}.apply(); }

int main()
{
    std::printf("%d %d\n", sum(), product());
    return sum() == 9 && product() == 30 ? 0 : 1;
}
