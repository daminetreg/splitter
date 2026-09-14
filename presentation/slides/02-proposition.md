---
chapter: Opening
chapter-label: Translation Unit (TU) Fission
notes: A translation unit is the atom of a C++ build — the smallest thing the compiler, the cache and the scheduler can take. Touch one function, the whole atom recompiles, misses the cache, goes out as one action. The proposition is to split the atom, one piece per function, while the build system still gets the object it asked for.
---
## {split}TU fission{/split} : splitting the {violet}atom{/violet} of builds.

::: fission
- 📄 TU.cpp\n  Original Translation Unit of work | violet
- 📄 add.cpp | split
- 📄 multiply.cpp | split
- 📄 greet.cpp | split
- 📄 average.cpp | split
- 📄 definitions.cpp | split
:::

Split the atom: one piece per function. Finer-grained compilation. Finer-grained caching. Finer-grained distribution