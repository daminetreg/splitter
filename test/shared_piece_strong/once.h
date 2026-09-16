#pragma once
// An implementation include: a non-inline definition beside an inline one, legal in exactly
// one unit. The inline once_fn() is a sharing candidate, but its shared piece compiles the
// whole header and the object carries strong_once() as a strong symbol -- which an object
// every includer links may not. The store must refuse it and the unit compile its own piece,
// for that reason and not because the platform's nm calls every symbol strong (TODO/54).
int strong_once(int v) { return v + 100; }
inline int once_fn(int v) { return strong_once(v) + 1; }
