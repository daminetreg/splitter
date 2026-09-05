#pragma once

template <class T>
struct holder {
    T items[4];
    int size;
};

// An ordinary function that must still be split, so the fixture cannot pass by the header
// being left alone wholesale.
inline int nl_tag() { return 1; }

#include "no_linkage_type_impl.ipp"
