---
chapter: Benchmarks
chapter-label: Set 3 / what changed
notes: The body edit as actions. Plain executes 158 remote compiles, one per unit that includes mat.inl.hpp. The split executes 23 -- the pieces whose text changed -- and takes 510 from the cache; the 158 units re-slice their twin without a parse. 139.1s against 14.5s in this run, 9.6x. The run before TODO/44 read 40.9s against 22.5s with 3 units compiled whole on every edit; now every unit re-slices.
---
## {split}158{/split} units include it.  
{accent}23{/accent} pieces compile.

::: metrics 3
- 158 |  | plain remote compiles
- 23 | accent | split remote compiles
- 510 | violet | split cached actions
:::

The body-edit comparison is 139.1s versus 14.5s
## Split is {split}9.6x faster{/split}
