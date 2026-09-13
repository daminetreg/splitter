#pragma once
// Found through -I, guarded, and written with LIB_API from defs.hpp. In a prefix PCH built
// where defs.hpp was not found, `class LIB_API Widget` is read as a variable named Widget of
// a class named LIB_API; the guard then keeps the main parse from reading it again, so that
// is what the unit is split against.
class LIB_API Widget {
public:
    explicit Widget(int v) : v_(v) {}
    int value() const;
    int v_;
};
inline int Widget::value() const { return v_ * 7; }
inline String label(const Widget& w) { return "widget:" + std::to_string(w.value()); }
