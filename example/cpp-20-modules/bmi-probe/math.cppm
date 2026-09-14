module;
#include <cstdio>
export module math;
export inline int twice(int v) { return v * 2; }
int helper_kept() { return 7; }
export int seven() { return helper_kept(); }
export int plus_one(int v);
export struct S { int m(); int n() { return 3; } };
int S::m() { return 5; }
export template <class T> T ident(T v) { return v; }
