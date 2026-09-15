---
chapter: splitter waltkthrough
chapter-label: 04 / Two preambles
notes: Section 4 — two preamble layers. Do not claim -fkeep-inline-functions.
---
## Declarations everywhere.  
{violet}One owner{/violet} for unique definitions.

::: cards 2
- use_mylib_preamble.h |  | Includes + declarations. Every piece includes it; its matching compiler PCH is reused. | preamble
- use_mylib.cpp_definitions.h |  | Kept external definitions, including main. Included by exactly one owner piece. | definitions
:::

::: rationale
Rationale — Share parsed declarations through a compiler PCH while assigning definitions that may exist once to a single, explicit object owner.
:::