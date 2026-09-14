---
chapter: Benchmarks
chapter-label: Benchmark set 1 / cluster −j500
notes: Set 1: -j500 end-to-end wall, one single run. Full is a cached-action overhead row, not cold compilation. Values are only comparable inside this set.
---
## End-to-end wall time.  
Five scenarios.

::: legend
- plain | 
- splitter | s
:::

::: tiny
277 Boost.Spirit programs · 279 translation units · AMD EPYC-Milan (32 cores, 122 GiB) · clang 13.0.0 · C++17 Release · EngFlow RBE over mTLS
:::

::: bars
- full | 32.1 | 456.0
- no-op | 12.0 | 17.7
- source touch | 11.9 | 13.9
- header touch | 12.0 | 13.8
- one body | 136.2 | 74.9
:::

::: tiny
Each bar is value ÷ this set’s 456.0s maximum, from a zero baseline. Full: cached actions only (1,641 plain / 15,885 split), not cold compilation.
:::