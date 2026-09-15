---
chapter: Build Atomiticity
chapter-label: Translation Unit (TU) Fission
notes: The move everyone already knows goes the other way — fusion. CMAKE_UNITY_BUILD=ON makes CMake write unity_N.cxx files that #include a batch of your sources — CMAKE_UNITY_BUILD_BATCH_SIZE of them, 8 by default — and compiles those instead. Eight compile edges become one. Every header is parsed once per batch instead of once per source, so the cold build is faster and the graph is smaller. The price is the atom got bigger — touch any one of the eight sources in the batch and the whole batch recompiles, the cache misses for all eight, and two sources that both had a static helper called `f` now collide in one translation unit. Fission is the same lever pulled the other way.
---
## {split}TU fission{/split} : Splitting TUs, Headers & Modules

::: mermaid-columns 2:3
flowchart LR
    h["📄 mylib.h"] --> c
    s["📄 use_mylib.cpp"] --> c
    sys["📚 &lt;string&gt; &lt;vector&gt;<br/>&lt;numeric&gt; &lt;algorithm&gt;"] --> c
    c(["⚙️ compile TU<br/>clang++ -c use_mylib.cpp"]) --> o["🧱 use_mylib.o"]
    o --> l(["🔗 link<br/>clang++ -o use_mylib"])
    l --> exe["🚀 use_mylib"]
---
flowchart LR
    h["📄 mylib.h"] --> L
    s["📄 use_mylib.cpp"] --> L
    sys["📚 &lt;string&gt; &lt;vector&gt;<br/>&lt;numeric&gt; &lt;algorithm&gt;"] --> L
    L(["⚙️ cpp-splitter clang++ -c use_mylib.cpp"])
    subgraph split ["use_mylib.o.split/ · inside the compile edge"]
        direction LR
        L --> pre["📄 use_mylib_preamble.h + PCH<br/>included by every piece"]
        L --> mh["📄 include/mylib.h<br/>declarations only"]
        L --> p1["📄 mylib.h_1_add.cpp"]
        L --> p2["📄 mylib.h_2_multiply.cpp"]
        L --> p3["📄 mylib.h_3_greet.cpp"]
        L --> p4["📄 mylib.h_4_average.cpp"]
        L --> p0["📄 use_mylib.cpp_0_definitions.cpp<br/>main"]
        p1 --> c1(["⚙️"]) --> o1["🧱 _1_add.o"]
        p2 --> c2(["⚙️"]) --> o2["🧱 _2_multiply.o"]
        p3 --> c3(["⚙️"]) --> o3["🧱 _3_greet.o"]
        p4 --> c4(["⚙️"]) --> o4["🧱 _4_average.o"]
        p0 --> c0(["⚙️"]) --> o0["🧱 _0_definitions.o"]
        o1 & o2 & o3 & o4 & o0 --> r(["🔗 ld -r"])
    end
    r --> o["🧱 use_mylib.o"]
    o --> l(["🔗 link<br/>clang++ -o use_mylib"]) --> exe["🚀 use_mylib"]

:::

::: list
- Use Cases
  - Faster builds on parallel distributed CPU cores
  - Faster edit-build-test loop
:::