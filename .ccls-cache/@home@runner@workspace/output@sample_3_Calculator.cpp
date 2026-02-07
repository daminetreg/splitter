// Function: Calculator::Calculator(int)
// Source: test/sample.cpp (lines 26-28)
// ---

#include "sample_preamble.h"

Calculator::Calculator(int initial) : value_(initial) {
  std::cout << "The Calculator created with value: " << initial << std::endl;
}
