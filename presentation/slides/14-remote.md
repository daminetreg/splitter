---
chapter: Architecture
chapter-label: 09 / Optional remote split
notes: Section 9 — emit-only splitter is invoked using rewrapper when remote configuration exists; tree returns through output directories and compiles/link follow.
---
## Move the parse,  
not the {accent}contract{/accent}.

::: flow
- local launcher | 
- rewrapper\ninput scan | violet
- worker: emit-only\nlibclang parse | split
- download split tree | teal
:::

::: rationale
Rationale — Send the same emit-only transformation to a worker when configured, while retaining local piece compilation and relocatable linking behavior.
:::