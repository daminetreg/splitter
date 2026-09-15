---
chapter: Architecture
chapter-label: 08 ½ / Modules · interface
notes: The interface unit is a source of pieces the way a header is. The rewrite keeps the module declaration, the global module fragment, every declaration, inline bodies and templates, and replaces each non-inline body by its declaration. That rewritten file is what the compiler precompiles, so the BMI holds no ODR hash of a body that lives elsewhere — byte-identical across body edits, measured in TODO/43.
---
## An interface is a source  
of {accent}pieces{/accent}, too.

::: flow
- foo.cxx\ndeclarations + bodies |  | module-original
- foo_interface.cxx\ndeclarations → foo.pcm | teal | module-interface
- foo.cxx_1_foo.cpp …\nfoo.cxx_3_helloworld.cpp | split
:::

The compiler precompiles the rewritten interface; a body edit leaves foo.pcm byte-identical.
