---
chapter: Binary evidence
chapter-label: Binary equality
notes: Bars share an explicit zero baseline and the canonical shared 0–8,880-byte scale. File delta is 184 bytes, approximately 2.12 percent. Stripping symbol tables yields same size but 385 bytes still differ due to dynamic-symbol ordering.
---
## Same binary  
different {split}metadata{/split}.

::: sizes
{
  "scale": 8880,
  "unit": "B",
  "plainLabel": "plain-gc",
  "splitLabel": "split-LTO+GC",
  "allLabel": "all metrics",
  "controls": ["executable", "text", "stripped"],
  "metrics": [
    {"name": "executable", "label": "executable", "plain": 8696, "split": 8880, "note": "+184 B · 2.12%"},
    {"name": "text", "label": ".text", "plain": 3314, "split": 3314, "note": "equal"},
    {"name": "data", "label": ".data", "plain": 584, "split": 584, "note": "equal"},
    {"name": "stripped", "label": "stripped", "plain": 6752, "split": 6752, "note": "equal size · 385 B differ"}
  ]
}
:::

::: tiny
22 symbols vs 22; normalized instructions 178 vs 178. Stripped: 6,752 B vs 6,752 B, yet 385 bytes differ in dynamic-symbol ordering.
:::