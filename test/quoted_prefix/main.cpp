// Regression fixture for TODO/42 (5): a quoted include of a sibling header in the include
// block, with no -I naming the source's directory -- the shape of OpenCV's
// `#include "precomp.hpp"`. Run as the launcher, not the command-line tool, because the tool
// puts the source directory on -I itself and so never saw this.
#include "defs.hpp"
#include "widget.hpp"
#define TWICE_DECLARE
#include "twice.hpp"
#undef TWICE_DECLARE
#define TWICE_DEFINE
#include "twice.hpp"
#undef TWICE_DEFINE
#include <cstdio>

int describe(const Widget& w) { return (int)label(w).size(); }

int main()
{
    Widget w(3);
    if (w.value() != 21) return 1;
    if (describe(w) != 9) return 2;
    if (twice_defined() != 30) return 3;
    std::printf("%d %d %d\n", w.value(), describe(w), twice_defined());
    return 0;
}
