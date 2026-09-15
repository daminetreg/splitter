---
chapter: Build Atomiticity
chapter-label: How-To
notes: Three ways to change the shape of the graph without changing the program. Fuse — unity builds, fewer and bigger atoms. Split — what this talk is about, more and smaller atoms, one per function. And reorganize by hand — forward declarations, pimpl, moving inline bodies out of headers, precompiled headers, modules — the case-specific work that a careful team does over years, and that the other two do mechanically.
---
## How to {violet}unity-{/violet} or {split}split-{/split} ?

::: list
- **Unity builds**
  - `cmake ... -DCMAKE_UNITY_BUILD=ON`
  -  Back in 2018 we built a deterministic tool to automatically handle _static_ types and functions, _anonymous namespace_ collisions
     -  tipi.build binary optimizer / libclang based "headerizer"
- **Split builds**
  - `cmake ... -DCMAKE_CXX_COMPILER_LAUNCHER=cpp-splitter`
  - :fa-github: `github.com/daminetreg/splitter`
:::
