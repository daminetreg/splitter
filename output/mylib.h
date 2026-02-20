#pragma once
#line 1 "/home/runner/workspace/test/mylib.h"
#pragma once
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

#line 9 "/home/runner/workspace/test/mylib.h"


#line 13 "/home/runner/workspace/test/mylib.h"


#line 17 "/home/runner/workspace/test/mylib.h"


#line 23 "/home/runner/workspace/test/mylib.h"


#line 25 "/home/runner/workspace/test/mylib.h"
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}
#line 28 "/home/runner/workspace/test/mylib.h"


int add(int a, int b);
int multiply(int a, int b);
std::string greet(const std::string& name);
double average(const std::vector<double>& values);
