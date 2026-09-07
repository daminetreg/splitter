#pragma once
#include "registry.hpp"

extern std::string param_name;






Registry& registry();




// The object whose construction does the registering.
static Registrar s_registrar;

// Enough functions that the unit splits into several pieces.
int helper_one();

int helper_two();

int helper_three();

int helper_four();


