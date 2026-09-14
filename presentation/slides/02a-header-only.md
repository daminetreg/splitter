---
chapter: Opening
chapter-label: Translation Unit (TU) Fission
notes: Before the atom, the thing every atom carries a copy of. Header-only libraries — Boost, Eigen, fmt, the test frameworks, most of what people write themselves — put function bodies in headers. mylib.h is the fixture the rest of the talk splits: inline functions and a template, included by every unit that uses them. Edit one body and every including translation unit recompiles, misses the cache, goes out again. That is the cost the fission goes after.
---
## {violet}Header only{/violet} libraries are everywhere.

::: code-columns
```cpp
// mylib.h
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
```
---
```cpp
inline std::string
greet(const std::string& name) {
    return "Hello, " + name + "!";
}
inline double
average(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = std::accumulate(
        v.begin(), v.end(), 0.0);
    return sum / v.size();
}
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}
```
:::
