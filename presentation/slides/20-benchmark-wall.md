---
chapter: Benchmarks
chapter-label: Benchmark set 1 / cluster −j500
notes: Set 1, 16 September 2026, after TODO/51 (one shared piece per header function): end-to-end wall on the EngFlow cluster at -j500, the split itself produced on workers. The plain rows are the 9 September ones, whole invocation, about 20s of mirroring and configuring included. Full is not cold compilation on either side — plain is 1641 cached actions; split is 4878 remote actions, 279 parses on workers plus every piece compiled without the local PCH, and the pieces of the remote splits downloaded: transfer and parsing, not compilation. The row that counts is the body edit — 136.2s against 38.8s, 3.5x. Values only comparable inside this set.
---
## End-to-end wall time.  
Three scenarios.

::: legend
- plain | 
- splitter | s
:::

::: tiny
277 Boost.Spirit programs · 279 translation units · AMD EPYC-Milan (32 cores, 122 GiB) · clang 13.0.0 · C++17 Release · EngFlow RBE over mTLS · cpp-splitter 8ab6e9f3, 16 Sep 2026
:::

::: bars
- full | 32.1 | 1096.6
- no-op | 12.0 | 24.3
- one body | 136.2 | 38.8
:::

::: tiny
Each bar is value ÷ this set’s 1096.6s maximum, from a zero baseline. Full: plain is 1,641 cached actions; split is 4,878 remote actions — 279 parses and every piece compiled on workers — plus 1,791 cached. Transfer and parsing, not cold compilation.
:::
