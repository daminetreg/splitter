#ifndef SPELLED_INCLUDE_API_H
#define SPELLED_INCLUDE_API_H
// Split: two inline functions. Included beside itself by wrapper.h, and by the unit as
// "src/api.h" through -I<this tree>. p4c's lib/cstring.h is spelled both ways (TODO/47).
inline int scale(int v) { return v * 3; }
inline int shift(int v) { return v + 4; }
#endif
