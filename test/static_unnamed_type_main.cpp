// Regression fixture for TODO/42 (4): a static variable of an unnamed struct type, as
// OpenCV's array.cpp declares `static struct { ... } CvIPL;`. Its type cannot be named from
// another translation unit, and every piece is one. Kept in the preamble it is one object per
// piece: the hook set here is not the hook read there, and the program below printed -1 from
// a split build that reported no fallback. Moved out, no declaration can name its type. The
// unit has to be compiled whole, and the splitter has to say so.
#include <cstdio>

static struct { int (*hook)(int); } Hooks;

static int doubled(int v) { return v * 2; }
void set_hook() { Hooks.hook = doubled; }
int call_hook(int v) { return Hooks.hook ? Hooks.hook(v) : -1; }

int main()
{
    set_hook();
    std::printf("%d\n", call_hook(21));
    return call_hook(21) == 42 ? 0 : 1;
}
