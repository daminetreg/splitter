---
chapter: splitter waltkthrough
chapter-label: Translation Unit (TU) Fission
notes: Before the atom, the thing every atom carries a copy of. Header-only libraries — Boost, Eigen, fmt, the test frameworks, most of what people write themselves — put function bodies in headers. mylib.h is the fixture the rest of the talk splits: inline functions and a template, included by every unit that uses them. Edit one body and every including translation unit recompiles, misses the cache, goes out again. That is the cost the fission goes after.
---
## use_mylib.cpp a {violet}monolihic TU{/violet}

```cpp
#include "mylib.h"

int main() {
  add(3, 4);
  multiply(5, 6);
  greet("World");

  std::vector<double> vals = {1.0, 2.0, 3.0, 4.0, 5.0};
  average(vals);

  max_of(10, 20);

    return 0;
}
```