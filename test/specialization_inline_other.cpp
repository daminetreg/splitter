#include "specialization_inline_header.hpp"

// A second translation unit, so the specializations really are carried by more than one
// object and the linker has to merge rather than reject them.
double other_unit() { return rounded<double>(-3.0) + scale<double>(2.0); }
