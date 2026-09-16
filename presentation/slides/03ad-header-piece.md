---
chapter: splitter waltkthrough
chapter-label: Header piece
notes: Each header piece includes the unit preamble before the rewritten header. It carries #line and used attribute; no claim about -fkeep-inline-functions.
---
## split-body pieces

```cpp
// include/mylib.h_1_add.cpp
#include "use_mylib_preamble.h"
#include "mylib.h"

__attribute__((used))
#line 7 "/…/mylib.h"
inline int add(int a, int b) { return a + b; }
```

::: rationale
Mirror included paths and compile each moved body under its consumer’s declaration context so resolution remains equivalent.
:::