---
chapter: Benchmarks
chapter-label: Benchmark set 4 / p4c, host −j16
notes: Set 4, build-only, one machine. Full: unity wins, 72.5 against 127.8 plain, and the split is 965.5 -- 10381 pieces, every function of every unit an object, with nothing to absorb it. No-op: nothing, all three. One source touched: plain 3.7, unity 10.0 -- the batch -- split 0.6, the unit reuses its split. One header touched: plain 106.8, unity 63.6, split 19.4: the copies are unchanged, so the launchers validate and relink. One body, findlast: 102.6, 63.9, 23.0. The split loses the cold build and wins every edit by 3 to 17x over unity.
---
## p4c local machine  
unity wins {violet}cold{/violet}, the split wins {split}every edit{/split}.

::: legend
- plain | 
- unity | u
- splitter | s
:::

::: bars
- full | 127.8 | 72.5 | 965.5
- one body | 102.6 | 63.9 | 23.0
:::

::: tiny
Build phase only, `-j16`, one run. One body is `cstring::findlast()`;
:::
