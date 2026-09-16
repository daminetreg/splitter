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

inline long heavy(int v) {
    // Thousands of class and function instantiations, each emitted: a dozen blocks of a
    // thousand, under the expression nesting limit a single fold would hit.
    long total = 0;
    total += sum_block<0>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<1000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<2000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<3000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<4000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<5000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<6000>(std::make_integer_sequence<int, 1000>{});
    total += sum_block<7000>(std::make_integer_sequence<int, 1000>{});
    return total % 1000 + v;
}

inline int light(int v) { return v + 1; }
