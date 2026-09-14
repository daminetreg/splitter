---
chapter: Benchmarks
chapter-label: Set 1 / what changed
notes: Set 1 actions: body edit executes 271 plain remote compiles vs 1 split remote compile, with 1575 cache hits. 194 units reach header and exactly one emits.
---
## {split}194{/split} units reach it.  
{accent}One{/accent} emits it.

::: metrics 3
- 271 |  | plain remote compiles
- 1 | accent | split remote compile
- 1575 | violet | split cached actions
:::

The body-edit comparison is 136.2s versus 74.9s, within this single end-to-end set.