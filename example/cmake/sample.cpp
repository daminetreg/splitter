#include <iostream>
#include <string>
#include <vector>

namespace math {

int add(int a, int b) {
    return a + b;
}

double multiply(double x, double y) {
    return x * y * y;
}

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

Calculator::Calculator(int initial) : value_(initial) {
    std::cout << "Calculator created with value: " << initial << std::endl;
}

Calculator::~Calculator() {
    std::cout << "Calculator destroyed" << std::endl;
}

int Calculator::getValue() const {
    return value_;
}

void Calculator::add(int x) {
    value_ += x;
}

void Calculator::reset() {
    value_ = 0;
}

template <typename T>
T maximum(T a, T b) {
    return (a > b) ? a : b;
}

static void helper_function() {
    std::cout << "This is a helper" << std::endl;
}

int main(int argc, char* argv[]) {
    Calculator calc(10);
    calc.add(5);
    std::cout << "Value: " << calc.getValue() << std::endl;

    std::cout << "Add: " << math::add(3, 4) << std::endl;
    std::cout << "Max: " << maximum(10, 20) << std::endl;

    helper_function();
    calc.reset();
    return 0;
}
