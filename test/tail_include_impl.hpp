// Not a standalone header: included at the end of tail_include_main.cpp, the way OpenCV's
// filesystem.cpp includes plugin_loader.impl.hpp. Out-of-line members with external linkage
// and no `inline`, so each may exist in exactly one object.
Loader::Loader() : v_(41) {}
Loader::~Loader() {}
int Loader::value() const { return v_ + 1; }
