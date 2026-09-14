---
chapter: Architecture
chapter-label: 07 / Four paths
notes: Section 7 — reuse, reslice, parse local, or remote split. A reslice is checked to write same tree as a full split for same edit.
---
## Most invocations  
should {accent}not parse{/accent}.

::: logic
- split.cache matches\n→ reuse | one body only? | harvest splice\n→ re-slice
:::

::: flow
- otherwise | 
- remote split if configured | violet
- or local libclang parse | 
:::