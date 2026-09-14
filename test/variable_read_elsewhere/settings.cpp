// Defines the two variables and reads neither: whatever the preamble makes of them has to
// reach the object anyway, since only main.cpp reads them.
#include "settings.h"

int Settings::tabsz = 2;
int counter = 5;

int unrelated(int v) { return v + 1; }
