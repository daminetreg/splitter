// Function: T max_of(T, T)
// Source: /home/runner/workspace/test/mylib.h (lines 25-28)
// Note: template - kept in preamble header for compilation
// ---

#include "mylib.h"

#line 25 "/home/runner/workspace/test/mylib.h"
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}
