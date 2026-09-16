---
chapter: Binary evidence
chapter-label: Binary equality
notes: Measurements only from example/binary-impact/README.md: 14 Sep 2026, clang 13.0.0, -O2, lld final linker. We compare the plain-gc baseline with the combined split-lto-relink-gc case—not an eight-way tour.
---
## Comparing {split}split{/split} to {violet}plain{/violet}  


::: flag
{violet}plain{/violet}  -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections  
{split}split{/split}  same flags + -flto=thin · CPP_SPLITTER_LINKER=ld.lld
:::

::: flow
- one plain TU | violet
- pieces → ld.lld -r\nper-TU ThinLTO | split
- final section GC | teal
:::

::: tiny
The baseline does {accent}not{/accent} use LTO. lld relinks the split unit to a native object; final linking does no LTO across units.
:::