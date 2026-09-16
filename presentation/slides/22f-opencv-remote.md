---
chapter: Benchmarks
chapter-label: Set 3 / what the splitter did, −j500
notes: The same run, as the splitter's own counts. Full -- 158 units split on the cluster, 160 remote actions: the parses and the pieces that were new since the run before; 26436 pieces came from the cache. One body -- 158 units re-sliced their twin here, 23 pieces recompiled on the cluster, none compiled whole. 0 fallbacks, 0 declined: after TODO/44 the three units that used to be compiled whole -- a static of an unnamed type, a class in an unnamed namespace, a header included twice -- split too.
---
## What the {split}splitter{/split} did.

::: single-bars
- full | 69.3 | 158 units split on the cluster · 160 remote actions · 26,436 pieces from the cache
- one body | 14.5 | 158 re-slices here · 23 pieces compiled · 0 compiled whole
:::

::: callout
Every unit splits: static of an unnamed type, a static of a class in an unnamed namespace, a header included twice under two macro states -- and the edit re-slices all 158.
:::
