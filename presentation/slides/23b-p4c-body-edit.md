---
chapter: Benchmarks
chapter-label: p4c
notes: The same kind of edit as on Spirit: one line in the body of an inline member of a header every unit includes. Two members are measured. findlast is emitted by 2 units and named by 11 more; the other 205 copies declare it only, so an edit to its body leaves their copies, their PCHs and their pieces alone. size is named by every unit -- size is a member of every container, and the rule that declares a function only is textual -- so every copy keeps its body and the edit reaches every PCH. For plain and unity the member makes no difference: every includer recompiles, or every batch.
---
## The edit: {accent}one line in `cstring::findlast()`{/accent}

```cpp
// lib/cstring.h
    // Search for characters. Linear time.
    const char *find(int c) const { return str ? strchr(str, c) : nullptr; }
    const char *findlast(int c) const { (void)4; return str ? strrchr(str, c) : str; }
                                        ^^^^^^^^  the edit
```

::: cards 3
- plain | | 218 units recompile, 102.6s: every one includes the header.
- unity | violet | 29 batches recompile, 63.9s: the header is in every batch.
- split | split | 2 units re-slice, 203 declare it only and change nothing: 2 PCHs, 50 pieces, 23.0s.
:::
