// No include guard and no include of its own: it compiles only after its includer has
// defined ctx_base. A shared piece built from the original alone cannot, so the unit
// compiles its own piece for it, as before TODO/51.
inline int ctx_fn(int v) { return ctx_base(v) + 1; }
