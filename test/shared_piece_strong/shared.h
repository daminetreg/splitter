#pragma once
// An inline function two units emit: shared through the store, one object for both.
inline int shared_fn(int v) { return v * 7; }
