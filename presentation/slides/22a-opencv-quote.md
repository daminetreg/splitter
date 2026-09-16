---
chapter: Benchmarks
layout: quote
notes: The second corpus on the cluster. OpenCV, core and imgproc, static, nothing optional: 158 C++ units of ordinary library code -- classes with members defined out of line, inline members in headers, no template metaprogramming to speak of. The opposite shape from Spirit.
---

**Benchmark**

## {violet}Plain{/violet} vs {split}Split{/split}  
on EngFlow RBE
 
{split}OpenCV{/split} core and imgproc

::: rationale
OpenCV is the open-source computer vision library: matrices, image processing, filtering, geometry. Its `core` and `imgproc` modules are 158 translation units of ordinary C++ with the classes' inline members in `mat.inl.hpp`, a header all of them include.
:::
