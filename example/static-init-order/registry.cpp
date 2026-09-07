#include "registry.hpp"

std::string param_name = "color_output";

void Registry::add(std::string const& n) { names.push_back(n); }

bool Registry::has(std::string const& n) const {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return true;
    return false;
}

Registry& registry() {
    static Registry the_one;
    return the_one;
}

Registrar::Registrar() { registry().add(param_name); }

// The object whose construction does the registering.
static Registrar s_registrar;

// Enough functions that the unit splits into several pieces.
int helper_one()   { return 1; }
int helper_two()   { return 2; }
int helper_three() { return 3; }
int helper_four()  { return 4; }
