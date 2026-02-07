// Function: int main(int, char **)
// Source: test/sample.cpp (lines 47-58)
// ---

#include "sample_preamble.h"

#line 47 "/home/runner/workspace/test/sample.cpp"
int main(int argc, char *argv[]) {
  Calculator calc(10);
  calc.add(5);
  std::cout << "Value: " << calc.getValue() << std::endl;

  std::cout << "Add: " << math::add(3, 4) << std::endl;
  std::cout << "Max: " << maximum(10, 20) << std::endl;

  __static_sample__helper_function();
  calc.reset();
  return 0;
}
