---
chapter: Architecture
chapter-label: 01 / Input
notes: Section 1 — input. The splitter is a CMAKE_CXX_COMPILER_LAUNCHER, invoked in place of the compiler.
---
## The build asks for  
{split}use_mylib.o{/split}.

```cpp
cpp-splitter clang++ -I. -MD -MF use_mylib.o.d -c -o use_mylib.o use_mylib.cpp
```

::: tree
use_mylib.o.split/  
├── {accent}prefix.h{/accent} · libclang PCH  
├── {accent}preamble.h{/accent} · compiler PCH  
├── definitions header + owner  
├── mirrored include/ tree  
├── one source + object per piece  
└── cache · hash · harvest · depfile
:::