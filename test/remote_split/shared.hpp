#pragma once
// Named nowhere in the rewrapper invocation. Reproxy's C++ input processor has to find it and
// upload it, which is the whole reason the action is labelled type=compile.
inline int shared_base() { return 40; }
