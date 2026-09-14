---
chapter: Architecture
chapter-label: 05 / Header mirror
notes: Section 5 — rewrite project headers that the unit reads, mirror by include path, and put that root first.
---
## A header is a source  
of {accent}pieces{/accent}, too.

::: flow
- mylib.h\noriginal bodies |  | header
- include/mylib.h\ndeclarations | teal | mirror
- _1_add.cpp …\n_4_average.cpp | split
:::

The split include root precedes project include directories.