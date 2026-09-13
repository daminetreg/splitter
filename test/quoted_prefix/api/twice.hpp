// No include guard, included twice from the include block under two macro states -- the
// shape of OpenCV's *.simd.hpp. The prefix PCH holds both inclusions. If the main parse then
// reads the include block from the source again, this file is processed a third and a
// fourth time and everything it declares is a `redefinition`.
#ifdef TWICE_DECLARE
enum TwiceMode { TWICE_A = 1, TWICE_B = 2 };
inline int twice_declared() { return TWICE_A + TWICE_B; }
#endif
#ifdef TWICE_DEFINE
inline int twice_defined() { return twice_declared() * 10; }
#endif
