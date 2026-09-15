---
chapter: Build Atomiticity
chapter-label: Translation Unit (TU) Fission
notes: Three ways to change the shape of the graph without changing the program. Fuse — unity builds, fewer and bigger atoms. Split — what this talk is about, more and smaller atoms, one per function. And reorganize by hand — forward declarations, pimpl, moving inline bodies out of headers, precompiled headers, modules — the case-specific work that a careful team does over years, and that the other two do mechanically.
---
## {violet}TUs{/violet} sizing

::: list
- Why are {violet}TUs{/violet} the size they are ?
  - intra-TU compiler optmization (Before LTO)
  - all function of class/module belong logically together
  - the developer (or the LLM) was just too lazy to create another file
:::

::: tiny
LTO: Link Time Optimization
:::