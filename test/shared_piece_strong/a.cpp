#include "shared.h"
#include "once.h"
int use_a(int v) { return shared_fn(v) + once_fn(v); }
