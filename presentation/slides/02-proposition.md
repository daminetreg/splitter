---
chapter: Opening
chapter-label: The proposition
notes: A translation unit is the atom of a C++ build — the smallest thing the compiler, the cache and the scheduler can take. Touch one function, the whole atom recompiles, misses the cache, goes out as one action. The proposition is to split the atom, one piece per function, while the build system still gets the object it asked for.
---
## The translation unit is the {accent}atom{/accent} of the build.

::: fission
- 📄 use_mylib.cpp\none unit of work | violet
- 📄 add.cpp | split
- 📄 multiply.cpp | split
- 📄 greet.cpp | split
- 📄 average.cpp | split
- 📄 definitions.cpp | split
:::

Split the atom: one piece per function. Finer to compile, finer to cache, finer to distribute.

::: tiny
The build still asks for use_mylib.o — and still gets it.
:::
