---
chapter: splitter waltkthrough
chapter-label: 05 / Header shadowing + mirroring
notes: Section 5 — rewrite project headers that the unit reads, mirror by include path, and put that root first.
---
## Header {violet}shadowing{/violet} + {split}splitting{/split}

::: flow
- mylib.h\noriginal bodies |  | header
- include/mylib.h\ndeclarations | teal | mirror
- _1_add.cpp …\n_4_average.cpp | split
:::

The split include root precedes project include directories.