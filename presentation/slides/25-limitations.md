---
chapter: Conclusions
chapter-label: Improvements
notes: Limitations: single runs, noise; chosen function reach-to-use 194:1; no general distribution; host has no per-action records; no runtime performance claim.
---
## Current Limitations

::: list
- There are no relocating linker on Windows 
  - Achievable but requires implementing a special object file merger
- Adding lines in a function above others requires renumbering (and recompiling all functions below)
- Too much split pieces produced that are never used in complex builds
  - Uses a lot more disk space than necessary
- This is a very early prototype
:::
