#ifndef SPELLED_INCLUDE_WRAPPER_H
#define SPELLED_INCLUDE_WRAPPER_H
// Nothing to split here; it includes the split header beside itself. Read as the
// original, its directive found the original api.h next to it and every piece got the
// definitions back. p4c's tools/ir-generator/ir-generator.h and irclass.h (TODO/47).
#include "api.h"
struct Wrapper {
    int scaled;
};
#endif
