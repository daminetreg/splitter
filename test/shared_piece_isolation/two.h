#pragma once
// Two inline functions two units emit. Each has its own shared piece in the store; an edit
// to one is one new object, and the other's object is not touched. TODO/51.
inline int f_of(int v) { return v * 3; }
inline int g_of(int v) { return v + 40; }
