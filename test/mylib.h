#pragma once
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

inline int add(int a, int b) {
    return a + b;
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
