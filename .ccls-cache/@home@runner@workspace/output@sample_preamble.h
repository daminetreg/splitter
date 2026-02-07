#pragma once
#include <iostream>
#include <string>
#include <vector>

namespace math {





}

class Calculator {
public:
    Calculator(int initial);
    ~Calculator();

    int getValue() const;
    void add(int x);
    void reset();

private:
    int value_;
};











template <typename T>
T maximum(T a, T b) {
    return (a > b) ? a : b;
}





namespace math { int add(int, int); }
namespace math { double multiply(double, double); }
void helper_function();
int main(int, char **);
