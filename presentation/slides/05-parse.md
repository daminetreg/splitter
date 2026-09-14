---
chapter: Architecture
chapter-label: 02 / Parse once
notes: Section 2 — parse. Prefix includes become a content-keyed libclang PCH and parse occurs once.
---
## Harvest extents,  
context, {accent}emission{/accent}.

::: flow
- include prefix | 
- libclang PCH\nhash + deps | teal
- one AST parse | 
- definitions + spans\nconditionals + reach | split
:::

::: rationale
Rationale — Cache the expensive include prefix by content and parse once to collect exact source extents and inclusion reachability.
:::