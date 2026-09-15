// Included after the unit's macros: OPEN_NS and CLOSE_NS are defined by the unit, across
// several lines each, and the include prefix the parse precompiles has to carry them whole.
#include <clocale>
#include <string>
#include "continued_define_header.hpp"

OPEN_NS

Thing::Thing(int v) : value_(v) {}

int Thing::twice() const
{
    // ::lconv resolves only if <clocale> was read at global scope.
    const lconv* lc = localeconv();
    return value_ * 2 + (lc ? 0 : 1) + static_cast<int>(std::string().size());
}

CLOSE_NS
