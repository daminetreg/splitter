#pragma once

// A quoted, directory-relative include. Resolved relative to the directory of the file doing
// the including -- which becomes the mirror once this header is rewritten, and the mirror has
// no copy of quoted_sibling.hpp. Nothing on the -I list can find it either: the include root
// is the directory above, so the unqualified name resolves from nowhere.
#include "quoted_sibling.hpp"

inline int quoted_split_value()
{
    return QuotedSiblingTag::value + 2;
}
