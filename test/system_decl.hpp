#pragma once
// Declared the way glibc's <stdlib.h> declares free(): in a system header, with an
// exception specification the definition will not repeat. p4c's gc.cpp defines
// `void free(void *)` over that declaration (TODO/47).
#pragma clang system_header
extern "C" int compute(int) throw();
