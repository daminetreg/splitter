---
chapter: Architecture
chapter-label: 02 / Parse once
notes: Section 2 — parse. Prefix includes become a content-keyed libclang PCH and parse occurs once.
---
## Harvest function body locations and {accent}emission{/accent}.

::: flow
- include prefix | 
- libclang PCH\nhash + deps | teal
- one AST parse | 
- definitions USR + bytes extent/location\n + whether inline body are called and hence should/should not be emitted | split
:::

::: rationale
USR = Unified Symbold Resolution, Clang AST unique entity identifier (_e.g._  `c:@N@demo@S@Widget@F@value#1` for `demo::Widget::value() const`)
:::