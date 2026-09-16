#pragma once
// A body that is long to compile beside one that is not. The shared piece of light() must
// not parse heavy()'s body -- it includes the store's copy of this header, where heavy() is
// a declaration -- and an edit to light() must not recompile heavy()'s object. TODO/51.
#include <utility>
template <int N> struct box {
    static constexpr long value = (N * 31L + 7) % 1000003;
    static long get() { return value + N; }   // one function instantiated and emitted per N
};
template <int... Is> long sum_boxes(std::integer_sequence<int, Is...>) {
    long total = 0;
    ((total += box<Is>::get()), ...);
    return total;
}
template <int Base, int... Is> long sum_block(std::integer_sequence<int, Is...>) {
    long total = 0;
    ((total += box<Base + Is>::get()), ...);
    return total;
}

// Blocks of 200: clang 13's expression nesting limit is 256 and a fold counts its operands,
// so a block of 1000 does not parse there (Apple's clang takes it). Forty blocks recurse.
template <int Base, int Blocks> long sum_many() {
    if constexpr (Blocks == 0) return 0;
    else return sum_block<Base>(std::make_integer_sequence<int, 200>{}) + sum_many<Base + 200, Blocks - 1>();
}

inline long heavy(int v) {
    // Thousands of class and function instantiations, each emitted: forty blocks of two
    // hundred, under the expression nesting limit a single fold would hit.
    return sum_many<0, 40>() % 1000 + v;
}

inline int light(int v) { return v + 1; }
