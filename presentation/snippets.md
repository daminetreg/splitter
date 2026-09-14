# Shared popup snippets

Each snippet is one source of truth. Its first line is `::: snippet id | title |
card description`; the following fenced Markdown code block remains literal.

::: snippet preamble | use_mylib_preamble.h | Includes and declarations reused by every piece.
```cpp
#pragma once
#include "mylib.h"
#include <iostream>

int main();
```
:::
::: snippet definitions | use_mylib.cpp_definitions.h | Kept external definitions, including main, have one owner.
```cpp
#pragma once
#include "use_mylib_preamble.h"

int main() { /* body verbatim */ }

// use_mylib.cpp_0_definitions.cpp
#include "use_mylib.cpp_definitions.h"
```
:::
::: snippet header | mylib.h (original) | Original inline bodies in the project header.
```cpp
#pragma once
inline int add(int a, int b) { return a + b; }
inline int multiply(int a, int b) { return a * b; }
inline std::string greet(const std::string& name) { return "Hello, " + name + "!"; }
inline double average(const std::vector<double>& values) { /* implementation in fixture */ }
template<typename T> T max_of(T a, T b) { return (a > b) ? a : b; }
```
:::
::: snippet mirror | include/mylib.h (rewritten mirror) | Declarations replace moved bodies under the same include path.
```cpp
#pragma once
int add(int a, int b);
int multiply(int a, int b);
std::string greet(const std::string& name);
double average(const std::vector<double>& values);
template<typename T> T max_of(T a, T b) { return (a > b) ? a : b; }
```
:::