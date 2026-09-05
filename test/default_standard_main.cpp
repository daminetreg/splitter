// Covers the language standard libclang parses at when the command line names none.
//
// The driver and libclang each fall back to their own default, and they need not agree:
// clang 13's driver defaults to gnu++14 while libclang, given the same arguments, parses at
// C++17. The splitter then harvests one branch of this #if and the compiler compiles the
// other -- so the branch the splitter removed from the preamble is not the branch that is
// there, and both definitions survive:
//
//     multiple definition of `answer()'
//
// The value is printed rather than asserted, because the right answer depends on the
// compiler: the test compares it against what the same compiler produces unsplit.
#include <cstdio>

#if __cplusplus >= 201703L
int answer() { return 17; }
#else
int answer() { return 14; }
#endif

int main() {
    std::printf("%d\n", answer());
    return 0;
}
