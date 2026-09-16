#include "shared.h"
inline int ctx_base(int v) { return v + 10; }
#include "needs_ctx.h"
int use_a(int v) { return shared_fn(v) + ctx_fn(v); }
