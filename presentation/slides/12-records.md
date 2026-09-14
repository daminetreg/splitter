---
chapter: Architecture
chapter-label: 07 / Records
notes: The records are depfile.cache, inputs.hash, split.cache, and harvest.
---
## Remember enough  
to change {split}only a body{/split}.

::: cards 3
- depfile.cache |  | Original prerequisites remain visible to the build.
- inputs.hash |  | Content hashes for prerequisites.
- harvest |  | Extents, gaps, body/signature hashes, piece ownership.
:::

::: rationale
Rationale — Record content and source geometry so a body-only change can recreate the same split tree without rebuilding the AST.
:::