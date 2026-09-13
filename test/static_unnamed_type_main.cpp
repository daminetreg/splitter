// Fixture for TODO/44 (A), from TODO/42 (4): a static variable of an unnamed struct type, as
// OpenCV's array.cpp declares `static struct { ... } CvIPL;`. Its type cannot be named from
// another translation unit, and every piece is one. Kept in the preamble it is one object per
// piece: the hook set here is not the hook read there, and the program below printed -1 from
// a split build that reported no fallback. Moved out, no declaration can name its type.
//
// The splitter keeps the variable and every function that uses it together, in the one
// translation unit that may hold single instances -- `set_hook`, `call_hook`, and `doubled`,
// whose address the hook takes -- and splits the rest.
#include <cstdio>

static struct { int (*hook)(int); } Hooks;

static int doubled(int v) { return v * 2; }
void set_hook() { Hooks.hook = doubled; }
int call_hook(int v) { return Hooks.hook ? Hooks.hook(v) : -1; }

// Untouched by the variable: split as usual.
int unrelated(int v) { return v + 1; }

int main()
{
    set_hook();
    std::printf("%d %d\n", call_hook(21), unrelated(41));
    return call_hook(21) == 42 && unrelated(41) == 42 ? 0 : 1;
}
