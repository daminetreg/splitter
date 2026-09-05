// Covers variable definitions carried into every split piece.
//
// The preamble is the source with the function bodies carved out; everything else is copied
// through, and every piece includes the preamble. So an ordinary variable with external
// linkage was defined once per piece:
//
//     multiple definition of `demo::counter'
//
// That was 31 of the 62 fallbacks in the wider Boost run -- namespace-scope variables and
// out-of-class static data members alike.
//
// The other rows of the table have to keep working, and each is here for a reason a linker
// error would not catch:
//
//   * `limit` is const at namespace scope, which is internal linkage in C++: a copy per
//     piece is legal, and moving it would make the pieces that read it stop compiling,
//     because a const is what a constant expression needs.
//   * `scale` is constexpr, same story, and it is used where a constant is required.
//   * `tag` is inline: vague linkage, mergeable, and it must stay visible.
//   * `hidden` has internal linkage through `static`; the copies are harmless.
//   * `Counter::live` is an out-of-class static data member, whose class already declares
//     it, so no `extern` may be written in its place.
//   * `pair_a`/`pair_b` share one declaration, which the source text gives no per-name
//     declaration for. It moves whole -- half of it moving would be worse than none -- and
//     the preamble gets an `extern` for each name, rebuilt from its type.
#include <iostream>

namespace demo {

int counter = 0;                    // external linkage: must move
const int limit = 100;              // internal linkage: must stay
constexpr int scale = 2;            // internal linkage, and needed as a constant
inline int tag = 7;                 // vague linkage: must stay
static int hidden = 5;              // internal linkage: must stay
int pair_a = 1, pair_b = 2;         // one declaration, two declarators: moves whole

struct Counter {
    static int live;
    int value;
};

int Counter::live = 0;              // external linkage, but the class declares it

}  // namespace demo

int bump() {
    ++demo::counter;
    demo::Counter::live += demo::scale;
    return demo::counter;
}

int drain() {
    int arr[demo::scale] = {demo::hidden, demo::tag};
    return demo::counter-- + arr[0] + arr[1] + (demo::limit - demo::limit);
}

int main() {
    bump();
    bump();
    int d = drain();
    int n = demo::counter + demo::Counter::live + d + demo::pair_a + demo::pair_b;
    std::cout << n << "\n";
    // counter 1, live 4, d = 2 + 5 + 7 = 14, pair_a 1, pair_b 2
    return n == 22 ? 0 : 1;
}
