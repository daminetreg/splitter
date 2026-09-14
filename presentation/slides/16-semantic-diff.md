---
chapter: Binary evidence
chapter-label: Binary / semantic symbol diff
notes: A coloured semantic diagram of measured source-file symbol naming—not raw nm output. The relink restores inlining. main is owned by the definitions piece; greet is weak in both; the template is not moved.
---
## Five source names  
versus {violet}one{/violet}.

::: semantic plain-gc | split-lto-relink-gc
```diff
use_mylib.cpp
= main
= greet [W]
= add / multiply / average
= folded into main
= max_of<T> stays in header
```
---
```diff
+use_mylib.cpp_0_definitions.cpp
= main
= greet [W]
+ mylib.h_1_add.cpp … _4_average.cpp
= add / multiply / average:
= inlined, weak copies GC’d
= max_of<T>: no moved piece
```
:::

::: tiny
SCHEMATIC: names and status from the report, not an exact raw symbol listing. {violet}Symbol-table string metadata: +184 B.{/violet}
:::