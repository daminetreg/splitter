---
chapter: Conclusions
notes: Closing: functional equivalence is a transformation requirement, supported by same output contract and the re-slice/full-split tree equality test; runtime performance is not benchmarked.
eyebrow: Thank You!
---
## Why it matters ?

**:fa-github: [github.com/daminetreg/splitter](https://github.com/daminetreg/splitter)**
::: tiny
⭐️ ✨ Github stars welcome, alongside issues and PRs !💫
:::

::: list
- **Very first** {violet}Cold split build{/violet} is **much much more work**
  - **BUT** code-changes do not trigger full rebuild anymore, **{split}Cache HIT rates increases drastically{/split}**
   
- {split}Iteration are much faster{/split}: between {split}**2.8x (p4c local)**{/split} to {split}**9.6x (OpenCV on EngFlow RBE)**{/split}
  - AI Agents do alot of isolated edit to verify their changes
    - This accelerates AI based  development in C++ drastically.

- Parallel caching compiler frontend for {violet}C++{/violet}
  - Scales horizontally
  - Works with any existing compiler
:::

