---
chapter: splitter waltkthrough
chapter-label: 08 / Recompose
notes: Section 8 — ld -r combines object pieces to ordinary object. The linker may be ld, mold, or ld.lld; all use -r.
---
## {split}5 objects{/split} builds in parallel  
{violet}1 ordinary object{/violet} out.

::: flow merge=5
- add.o | split
- multiply.o | split
- greet.o | split
- average.o | split
- definitions.o | violet
- {mold,lld,ld} -r | teal
- use_mylib.o | 
:::

::: rationale
Use relocatable linking to return precisely the conventional object file expected by every downstream build step.
:::