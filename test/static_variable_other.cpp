#include <iostream>

// Spelled exactly as the other unit spells it. Both lose their internal linkage when they are
// moved, so the mangle is what keeps them apart.
static int same_name = 11;

int other_unit_value() { return same_name; }
