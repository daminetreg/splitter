// Regression fixture for TODO/05: member functions defined inside a class body must be
// emitted as out-of-line definitions, and the class must keep a declaration in their place.
#include <iostream>
#include <string>

namespace demo {

struct Base {
    virtual ~Base() {}
    virtual int kind() const { return 0; }
};

class Widget : public Base {
public:
    explicit Widget(int v, const std::string& tag = "w")
        : Base(), value_(v), tag_(tag) {}

    ~Widget() override {}

    int kind() const override { return 1; }

    int value() const { return value_; }

    static int scale(int v, int by = 2) { return v * by; }

    const std::string& tag() const { return tag_; }

    // Nested class: its members qualify as Widget::Inner::.
    struct Inner {
        int n;
        int doubled() const { return n * 2; }
    };

private:
    int value_;
    std::string tag_;
};

// Members of a class template cannot be defined out-of-line in a .cpp and must stay put.
template <typename T>
class Holder {
public:
    explicit Holder(T t) : t_(t) {}
    T get() const { return t_; }
private:
    T t_;
};

}  // namespace demo

int main() {
    demo::Widget w(21);
    demo::Widget::Inner in{4};
    demo::Holder<int> h(3);
    int n = w.value() + w.kind() + demo::Widget::scale(5) + in.doubled()
            + static_cast<int>(w.tag().size()) + h.get();
    std::cout << n << "\n";
    return n == 44 ? 0 : 1;
}
