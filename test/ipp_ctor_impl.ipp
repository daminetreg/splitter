// A constructor and destructor defined in an implementation include, which is a header in
// every way that matters: the file is included into a translation unit, not compiled as
// one. Boost writes utf8_codecvt_facet this way, and three libraries include it.
inline Counter::Counter(int start, int* marks) : value(start), sink(marks) { *sink += 1; }

inline Counter::~Counter() { *sink += 10; }
