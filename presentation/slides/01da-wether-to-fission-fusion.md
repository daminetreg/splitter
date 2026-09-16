---
chapter: Build Atomiticity
chapter-label: Wether-To
notes: Three ways to change the shape of the graph without changing the program. Fuse — unity builds, fewer and bigger atoms. Split — what this talk is about, more and smaller atoms, one per function. And reorganize by hand — forward declarations, pimpl, moving inline bodies out of headers, precompiled headers, modules — the case-specific work that a careful team does over years, and that the other two do mechanically.
---
## To {violet}unity-{/violet} or {split}split-{/split} build ?

::: list
- **It depends** : 
  - Compiling for an old platform without LTO ?
  - Build farm available, or just a 12-cores laptop ?
- **Unity builds**
  - **Requires** TUs to be concatenable:
    -  _static_ types and functions, _anonymous namespace_ : **collisions**.
- **Split builds**
  - Copy/paste manually definitins from headers, adapt declaration, cut translation units in smaller ones.
    - Or Ask your AI Agent to burn tokens?
    - Will the new file layout make any sense architecturally / logically anymore ?
    - What if  an Unity Build later is useful ?
:::
