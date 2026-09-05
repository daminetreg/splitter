#include "ipp_ctor_header.hpp"
#include <iostream>

int main() {
    int marks = 0;
    int n = 0;
    {
        Counter c(21, &marks);   // needs the complete-object constructor
        n = c.doubled() + marks;
    }
    n += marks;
    std::cout << n << "\n";
    return n == 54 ? 0 : 1;
}
