---
chapter: Benchmarks
chapter-label: Benchmark set 3 / OpenCV, cluster −j500
notes: Set 3: OpenCV on the cluster, -j500, build phase only, the split produced on the cluster. Full: 123.8 plain against 69.3 split -- both served from cache, 516 hits against 26436, so the row is the bookkeeping of 50 times more actions, and the split's parses are the 160 executions. One body: 139.1 plain against 14.5 split, 158 remote compiles against 23. The plain rows of this run were three times slower than the run before for the same action counts; the split rows are read against the plain rows of the same run.
---
## OpenCV on the cluster:  
{split}9.6x{/split} on the edit.

::: legend
- plain | 
- splitter | s
:::

::: tiny
158 C++ units · OpenCV 4.11.0 core and imgproc · AMD EPYC-Milan driving · clang 13.0.0 · C++17 Release · EngFlow RBE over mTLS, −j500 · split produced on the cluster
:::

::: bars
- full | 123.8 | 69.3
- one body | 139.1 | 14.5
:::

::: tiny
516 TUs plain / 26,436 split
:::
