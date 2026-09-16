---
chapter: Benchmarks
chapter-label: Set 1 / what changed
notes: Set 1 actions, the body edit. standard_wide::toucs4() is edited; 268 units include its header. Plain executes 271 remote compiles, one per includer. Split executes 51 — the header's 15 sharable functions and 36 of three headers whose include closure reaches it, once each — and the 268 units link the store's objects; 1575 actions come from the cache. The day before, with one piece per includer, the same edit was 267 remote actions and 129.8s. 136.2s against 38.8s, 3.5x.
---
## {split}268{/split} units reach it.  
{accent}51{/accent} compiles, shared by all.

::: metrics 3
- 271 |  | plain remote compiles
- 51 | accent | split remote compiles
- 1575 | violet | split cached actions
:::

The body-edit comparison is 136.2s versus 38.8s within this single end-to-end set — {split}3.5x faster{/split}
