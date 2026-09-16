---
chapter: Build Atomiticity
chapter-label: Translation Unit (TU) Fission
notes: The move everyone already knows goes the other way — fusion. CMAKE_UNITY_BUILD=ON makes CMake write unity_N.cxx files that #include a batch of your sources — CMAKE_UNITY_BUILD_BATCH_SIZE of them, 8 by default — and compiles those instead. Eight compile edges become one. Every header is parsed once per batch instead of once per source, so the cold build is faster and the graph is smaller. The price is the atom got bigger — touch any one of the eight sources in the batch and the whole batch recompiles, the cache misses for all eight, and two sources that both had a static helper called `f` now collide in one translation unit. Fission is the same lever pulled the other way.
---
## {violet}TUs fusion{/violet} : Unity Builds

::: mermaid-columns
flowchart LR
    sa["📄 a.cpp"] --> ca(["⚙️"]) --> oa["🧱 a.o"]
    sb["📄 b.cpp"] --> cb(["⚙️"]) --> ob["🧱 b.o"]
    sc["📄 c.cpp"] --> cc(["⚙️"]) --> oc["🧱 c.o"]
    sd["📄 d.cpp"] --> cd(["⚙️"]) --> od["🧱 d.o"]
    se["📄 e.cpp"] --> ce(["⚙️"]) --> oe["🧱 e.o"]
    sf["📄 f.cpp"] --> cf(["⚙️"]) --> of["🧱 f.o"]
    sg["📄 g.cpp"] --> cg(["⚙️"]) --> og["🧱 g.o"]
    sh["📄 h.cpp"] --> ch(["⚙️"]) --> oh["🧱 h.o"]
    oa & ob & oc & od & oe & of & og & oh --> l(["🔗 link"]) --> exe["🚀 app"]
---
flowchart LR
    u0["📄 unity_0.cxx<br/>#include a.cpp<br/>#include b.cpp<br/>#include c.cpp<br/>#include d.cpp<br/>#include e.cpp<br/>#include f.cpp<br/>#include g.cpp<br/>#include h.cpp"] --> c0(["⚙️"]) --> o0["🧱 unity_0.o"]
    o0 --> l(["🔗 link"]) --> exe["🚀 app"]
:::

::: list
- Use Cases
  - Before LTO: Permitted more intra-TU optimization
  - Faster builds on limited CPU cores count: amortizes preprocessing
:::

