// Regression fixture for TODO/42 (2): a file-static function whose unqualified name is also
// the name of a function in another namespace, and a macro that names that other function
// by its token. OpenCV's CPU dispatch is written this way:
//
//     static BinaryFunc getCvtScaleAbsFunc(int depth) {
//         CV_CPU_DISPATCH(getCvtScaleAbsFunc, (depth), ...);   // -> cpu_baseline::getCvtScaleAbsFunc
//     }
//
// The splitter renames a static so the pieces can share it, and applies the rename to every
// whole-identifier occurrence of the name. The token inside the macro argument is one, and
// after expansion it names `cpu_baseline::getCvtScaleAbsFunc`, which was never renamed:
// `no member named '__static_..._getCvtScaleAbsFunc' in namespace 'cpu_baseline'`.
#include <cstdio>

namespace lib {

namespace baseline {
int scale(int v) { return v * 3; }
}

// The dispatcher: expands to a qualified call of the *other* function of that name.
#define DISPATCH(fn, args) return baseline::fn args

static int scale(int v)
{
    DISPATCH(scale, (v));
}

int use_scale(int v) { return scale(v) + 1; }
int use_scale_twice(int v) { return scale(scale(v)); }

}  // namespace lib

int main()
{
    if (lib::use_scale(2) != 7) return 1;
    if (lib::use_scale_twice(2) != 18) return 2;
    std::printf("%d %d\n", lib::use_scale(2), lib::use_scale_twice(2));
    return 0;
}
