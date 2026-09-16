---
chapter: Benchmarks
chapter-label: The edit, on OpenCV
notes: The same edit as on Spirit: one line in the body of an inline member defined in a header every unit includes. SparseMat::nzcount is defined inline in mat.inl.hpp, included by all 158 units, and emitted by one -- matrix_sparse.cpp, which calls it. The other 157 carry it in their copy, or declare it only. A plain build recompiles all 158 units; the split build re-slices 158 twins and recompiles 23 pieces. What each did with the same edit is the next two slides.
---
## The edit: {accent}one line in `SparseMat::nzcount()`{/accent},  
inline in a header all 158 units include.

```cpp
// modules/core/include/opencv2/core/mat.inl.hpp
inline
size_t SparseMat::nzcount() const
{
    (void)4;  // benchmark probe   <- the edit
    return hdr ? hdr->nodeCount : 0;
}
```

::: cards 2
- plain | | 158 units recompile on the cluster, 139.1s: every one includes the header.
- split, produced on the cluster | split | the same 158 re-slices and 23 pieces, 14.5s; the parse never runs here.
:::

::: tiny
`nzcount` is called by `matrix_sparse.cpp` alone; the other 157 copies keep or declare it.
:::
