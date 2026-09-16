#pragma once
// An inline function two units emit: one shared piece in the store, linked by both (TODO/51).
inline int shared_fn(int v) { return v * 7; }
