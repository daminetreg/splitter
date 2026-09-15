---
chapter: Opening
chapter-label: Translation Unit (TU) Fission
notes: Three ways to change the shape of the graph without changing the program. Fuse — unity builds, fewer and bigger atoms. Split — what this talk is about, more and smaller atoms, one per function. And reorganize by hand — forward declarations, pimpl, moving inline bodies out of headers, precompiled headers, modules — the case-specific work that a careful team does over years, and that the other two do mechanically.
---
## Build graph {accent}optimization{/accent} techniques.

::: list
- **Unity builds** — fuse translation units: fewer, bigger atoms.
- **Splitting** — fission: more, smaller atoms, one per function.
- **Reorganizing** in case-specific ways — forward declarations, pimpl, bodies out of headers, PCH, modules.
:::
