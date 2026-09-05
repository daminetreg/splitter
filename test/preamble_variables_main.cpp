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
//   * `Pool<M>::slots_` is defined out of line with a `template<...>` prefix that sits
//     outside the cursor's extent. Moving it would strand the prefix in the preamble.
//   * `inline_type` is declared by `struct Inline { ... } inline_type;`, one declaration of
//     a type and a variable. The type has to stay -- every piece needs it, and a member
//     function defined in it is a definition of its own -- while the variable has to go, so
//     only the declarator moves.
//   * `arr` is an array: its type is spelled around its name, so the `extern` for it can
//     only come from the source text, never from the type.
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

int arr[3] = {1, 2, 3};             // the type is spelled around the name

template <int M>
struct Pool {
    static int slots_[2];
    static int total() { return slots_[0] + slots_[1]; }
};
// The `template<...>` prefix is outside the definition's extent.
template <int M> int Pool<M>::slots_[2] = {1, 1};

struct Inline {
    int v;
    Inline() : v(4) {}
    int half() const { return v / 2; }
} inline_type;                      // a type and a variable in one declaration

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
    int n = demo::counter + demo::Counter::live + d + demo::pair_a + demo::pair_b
            + demo::arr[0] + demo::arr[1] + demo::arr[2]
            + demo::Pool<0>::total() + demo::inline_type.half();
    std::cout << n << "\n";
    // counter 1, live 4, d = 2 + 5 + 7 = 14, pair_a 1, pair_b 2, arr 6, Pool 2, half 2
    return n == 32 ? 0 : 1;
}
