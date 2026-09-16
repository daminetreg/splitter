#pragma once
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cstdint>

inline int add(int a, int b) {
    return a + b; // and c
}

inline int multiply(int a, int b) {
    return a * b;
}

inline std::string greet(const std::string& name) {
    return "Hello, " + name + "!";
}

inline double average(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}


template <int Depth, std::uint64_t Path>
struct Burn {
    static constexpr std::uint64_t value =
        Burn<Depth - 1, Path * 2>::value ^
        Burn<Depth - 1, Path * 2 + 1>::value ^
        Path;
};

template <std::uint64_t Path>
struct Burn<0, Path> {
    static constexpr std::uint64_t value =
        Path * 0x9e3779b97f4a7c15ULL;
};

inline int terrible_depth() {
  // Increase/decrease this to tune compile time.
  // 18 => ~524k template instantiations
  // 19 => ~1.05m
  // 20 => ~2.1m
  constexpr auto result = Burn<18, 1>::value;
  return static_cast<int>(result);
}