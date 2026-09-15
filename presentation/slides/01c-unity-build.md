---
chapter: Opening
chapter-label: Translation Unit (TU) Fission
notes: The move everyone already knows goes the other way — fusion. CMAKE_UNITY_BUILD=ON makes CMake write unity_N.cxx files that #include a batch of your sources — CMAKE_UNITY_BUILD_BATCH_SIZE of them, 8 by default — and compiles those instead. A hundred compile edges become twelve. Every header is parsed once per batch instead of once per source, so the cold build is faster and the graph is smaller. The price is the atom got bigger — touch any one of the nine sources in a batch and the whole batch recompiles, the cache misses for all nine, and two sources that both had a static helper called `f` now collide in one translation unit. Fission is the same lever pulled the other way.
---
## Or {violet}fuse{/violet} them: unity builds.

::: mermaid-columns
flowchart LR
    s1["📄 a.cpp"] --> c1(["⚙️"]) --> o1["🧱 a.o"]
    s2["📄 b.cpp"] --> c2(["⚙️"]) --> o2["🧱 b.o"]
    s3["📄 c.cpp"] --> c3(["⚙️"]) --> o3["🧱 c.o"]
    dots["⋮  ×100 sources · 100 compile edges"]
    s100["📄 zz.cpp"] --> c100(["⚙️"]) --> o100["🧱 zz.o"]
    o1 & o2 & o3 & o100 --> l(["🔗 link"]) --> exe["🚀 app"]
---
flowchart LR
    u0["📄 unity_0.cxx<br/>#include a.cpp … i.cpp"] --> c0(["⚙️"]) --> o0["🧱 unity_0.o"]
    u1["📄 unity_1.cxx<br/>#include j.cpp … r.cpp"] --> c1(["⚙️"]) --> o1["🧱 unity_1.o"]
    dots["⋮  12 batches of 9 · 12 compile edges"]
    u11["📄 unity_11.cxx<br/>#include … zz.cpp"] --> c11(["⚙️"]) --> o11["🧱 unity_11.o"]
    o0 & o1 & o11 --> l(["🔗 link"]) --> exe["🚀 app"]
:::

**CMAKE_UNITY_BUILD=ON** writes unity_N.cxx files that #include **CMAKE_UNITY_BUILD_BATCH_SIZE** sources each (8 by default, 9 here). Headers parsed once per batch, a smaller graph — and a bigger atom: one edit recompiles the whole batch.
