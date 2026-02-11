#pragma once
#line 1 "/home/runner/workspace/test/sample.cpp"
#include <iostream>
#include <string>
#include <vector>

namespace math {

#line 7 "/home/runner/workspace/test/sample.cpp"


#line 9 "/home/runner/workspace/test/sample.cpp"


} // namespace math

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

#line 28 "/home/runner/workspace/test/sample.cpp"


#line 33 "/home/runner/workspace/test/sample.cpp"


#line 35 "/home/runner/workspace/test/sample.cpp"


#line 37 "/home/runner/workspace/test/sample.cpp"


#line 39 "/home/runner/workspace/test/sample.cpp"


#line 41 "/home/runner/workspace/test/sample.cpp"
template <typename T> T maximum(T a, T b) { return (a > b) ? a : b; }
#line 41 "/home/runner/workspace/test/sample.cpp"


#line 45 "/home/runner/workspace/test/sample.cpp"


#line 58 "/home/runner/workspace/test/sample.cpp"


namespace math { int add(int, int); }
namespace math { double multiply(double, double); }
void __static_sample__helper_function();
int main(int, char **);
