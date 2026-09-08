#pragma once

// The other half of the rule: this include names a header that is mirrored too. The mirror is
// structurally parallel to the original tree, so the relative path still points at the right
// file and the directive must be left exactly as written. Rewriting it to the original would
// read a header whose definitions have been split out of it.
#include "quoted_partner.hpp"

inline int quoted_pair_value()
{
    return quoted_partner_value() + 1;
}
