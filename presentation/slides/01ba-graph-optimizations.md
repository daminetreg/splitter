---
chapter: Build Atomiticity
chapter-label: Translation Unit (TU) Fission
notes: Three ways to change the shape of the graph without changing the program. Fuse — unity builds, fewer and bigger atoms. Split — what this talk is about, more and smaller atoms, one per function. And reorganize by hand — forward declarations, pimpl, moving inline bodies out of headers, precompiled headers, modules — the case-specific work that a careful team does over years, and that the other two do mechanically.
---
## Build graph {split}optimization{/split} techniques.

::: list
- **Unity builds** — {violet}TUs fusion{/violet}: fewer, bigger TUs.
  - concatenating TUs
- **Splitting / Partitioning / Sharding** — {split}TUs fission{/split}: more, smaller TUs.
  - splitting TUs per function/function groups
  - taking bodies out of headers
  - splitting headers : forward declaration to limit inclusions (c.f. Fast Kernel Headers Tree v1 - Ingo Molnar 2021)
:::
