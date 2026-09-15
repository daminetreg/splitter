---
chapter: Architecture
chapter-label: 08 ½ / Modules · piece
notes: Each piece is an implementation unit of the module. It imports its interface implicitly through the BMI, so there is no preamble of declarations to include — but the global module fragment is not visible through an import, so the piece replays it, ahead of the module declaration, from foo_preamble.h. #line points diagnostics back at foo.cxx. export cannot appear in an implementation unit, so the body carries no export and the declaration in the interface keeps it.
---
## Interface through the {violet}BMI{/violet}.  
Fragment through the {accent}preamble{/accent}.

```cpp
// foo.cxx.o.split/foo.cxx_3_helloworld.cpp
module;
#include "foo_preamble.h"     // the global module fragment: <iostream>
module foo;                   // implementation unit: imports foo.pcm implicitly

#line 17 "/…/foo.cxx"
void foo::helloworld() { std::cout << "hello world\n"; }
```

::: rationale
Rationale — Compile each moved body as an implementation unit under its own interface, with the global module fragment replayed, so lookup is the one the original unit had and the BMI never carries the body.
:::
