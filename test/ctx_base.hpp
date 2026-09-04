// Part of the TODO/09 fixture: completes the type that ctx_user.hpp relies on.
#pragma once

namespace demo {

struct Weight {
    int grams;
    int doubled() const { return grams * 2; }
};

}  // namespace demo
