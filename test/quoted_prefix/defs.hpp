#pragma once
// A sibling of main.cpp, included by name and resolved against the unit's own directory;
// nothing puts that directory on -I. It defines the macro the next header in the include
// block is written with.
#define LIB_API __attribute__((visibility("default")))
#include <string>
typedef std::string String;
