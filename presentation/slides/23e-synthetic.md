---
chapter: Benchmarks
chapter-label: Benchmark set 5 / generated corpus, host −j16
notes: The last corpus is generated, and it is the one the splitter was made for: 200 translation units of 40 free functions each, every function with external linkage and one declaration in its unit's header, bodies of a few lines of vector, map and ostringstream work, one common header with the standard headers and two inline helpers. No template of the project's own, nothing static: every definition moves into a piece. It bounds both sides -- the most the split can gain on an edit, and the most it costs cold.
---
## A generated corpus: {accent}8,000 free functions{/accent},  
every one splittable.

::: code-columns
```cpp
// unit_100.cpp, one of 200
#include "synthetic.h"
#include "unit_100.h"

int unit_100_fn_20(int x)
{
    std::vector<int> v;
    for (int i = 0; i < x % 11 + 3; ++i)
        v.push_back(mix(i, 72));
    std::map<std::string, int> m;
    m[tag(x)] = static_cast<int>(v.size());
    std::sort(v.begin(), v.end());
    std::ostringstream os;
    os << x << ':' << m.size() << ':' << v.front();
    return static_cast<int>(os.str().size())
         + std::accumulate(v.begin(), v.end(), 0) + 20;
}
// ... 39 more
```
---
```cpp
// synthetic.h, included by every unit
#pragma once
#include <algorithm>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

inline int mix(int a, int b) {
    return (a * 31 + b) % 1000003;
}
inline std::string tag(int v) {
    return "v" + std::to_string(v);
}

// unit_100.h
int unit_100_fn_0(int x);
// ... 39 more declarations
```
:::

::: tiny
`example/synthetic/generate.py 200 40`, deterministic · plain, unity (batches of 8), split · the edit: one line added to the body of `unit_100_fn_20()`
:::
