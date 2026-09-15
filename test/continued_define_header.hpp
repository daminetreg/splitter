#pragma once
// The class whose members continued_define_impl.ipp defines under a namespace-opening
// macro written with line continuations.
namespace outer { namespace inner {
struct Thing {
    explicit Thing(int v);
    int twice() const;
    int value_;
};
}}
