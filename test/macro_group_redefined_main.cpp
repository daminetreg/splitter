// One list macro invoked twice, each time with the helper macro defined differently and
// undefined after: p4c's visitor.cpp invokes IRNODE_ALL_SUBCLASSES(DEFINE_VISIT_FUNCTIONS)
// twice. Both groups define external members and cannot stay in the preamble; moved to the
// definitions header they would be re-expanded behind the whole preamble, where the helper
// is undefined. The definitions piece compiles a variant of the unit instead (TODO/47).
#include <cstdio>

struct Plus { int v; int apply() const; int twice() const; };
struct Neg { int v; int apply() const; int twice() const; };

#define ALL_OPS(M) M(Plus, +v) M(Neg, -v)

#define DEFINE_OP(NAME, EXPR) int NAME::apply() const { return EXPR; }
ALL_OPS(DEFINE_OP)
#undef DEFINE_OP

#define DEFINE_OP(NAME, EXPR) int NAME::twice() const { return 2 * (EXPR); }
ALL_OPS(DEFINE_OP)
#undef DEFINE_OP

int sum() { return Plus{3}.apply() + Neg{4}.apply(); }
int sum2() { return Plus{3}.twice() + Neg{4}.twice(); }

int main()
{
    std::printf("%d %d\n", sum(), sum2());
    return sum() == -1 && sum2() == -2 ? 0 : 1;
}
