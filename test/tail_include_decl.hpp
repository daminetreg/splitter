#pragma once
// The declarations, included at the top as usual. The out-of-line definitions arrive through
// tail_include_impl.hpp, included at the very end of the source.
struct Loader {
    Loader();
    ~Loader();
    int value() const;
    int v_;
};
