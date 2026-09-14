---
chapter: Architecture
chapter-label: 01 / Contract
notes: Rationale, section 1: keep the output at the path requested by the build so downstream tools remain unaware.
---
## Same compiler command.  
Different interior.

::: flow
- build asks use_mylib.o | violet
- cpp-splitter | 
- split tree + pieces | split
- use_mylib.o | violet
:::

::: paths
{"cache match":"reuse — use the existing split tree; recompile nothing; touch the requested object.","one body changed":"re-slice — splice the recorded changed body, renumber #line, and rewrite the harvest without parsing.","other input change":"parse — when inputs differ beyond a recorded body, parse with libclang locally or use the configured remote emit-only path."}
:::

::: rationale
Rationale — Preserve the requested object and dependency contract while making function-sized compilation units available to the scheduler.
:::