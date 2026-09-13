#pragma once
// A header with an inline function the unit calls, so the split writes a header piece. That
// piece includes the unit's preamble -- and if the preamble still carries the unit's extern "C"
// definitions verbatim, the piece emits them a second time.
inline int helper_twice(int v) { return v * 2; }
