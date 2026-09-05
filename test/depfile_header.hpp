// Split out of dep_main.cpp, so every piece includes the rewritten copy of this file rather
// than this file. The build system has to be told about *this* path all the same, or editing
// it rebuilds nothing and the stale object is linked while the build reports success.
#pragma once

namespace demo {

inline int value() { return 1; }

}  // namespace demo
