#pragma once
// One macro invocation, several definitions with external linkage and no `inline` -- the
// shape of OpenCV's DEFINE_SIMD_ALL(recip, ...) in arithm.simd.hpp. Every function reports
// the invocation as its extent, so none can be moved on its own, and the group as a whole
// may exist in exactly one object.
namespace lib { namespace kernels {

#define DEFINE_KERNELS(name, op)                                    \
    int name##8u(int a, int b) { return (a op b) & 0xff; }          \
    int name##16u(int a, int b) { return (a op b) & 0xffff; }       \
    int name##32s(int a, int b) { return a op b; }

DEFINE_KERNELS(add, +)
DEFINE_KERNELS(sub, -)

inline int helper_twice(int v) { return v * 2; }

}}  // namespace lib::kernels
