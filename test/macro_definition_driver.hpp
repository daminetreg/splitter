#pragma once
#include "macro_definition_header.hpp"
#include "macro_definition_impl.ipp"

// A definition of its own, so this header is rewritten too and goes on naming the rewritten
// copy of the .ipp rather than the original.
inline int driver_tag() { return 1; }
