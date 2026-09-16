---
chapter: splitter waltkthrough
chapter-label: Two preambles
notes: Section 4 — two preamble layers. Do not claim -fkeep-inline-functions.
---
## PCH preamble
{split}Parse common includes and declarations **once**{/split} across split-body pieces

::: cards 2
- use_mylib_preamble.h | split | **Common Includes + declarations**. Every split-body includes it; its matching compiler PCH is reused. | preamble
- use_mylib.cpp_definitions.h | violet | Kept external definitions, including main. Included by exactly one owner piece. | definitions
:::

::: rationale
Share parsed declarations through a compiler PCH while assigning unique definitions that may exist once to a single, explicit object owner.
:::

::: tiny
PCH = Pre Compiled Header
:::