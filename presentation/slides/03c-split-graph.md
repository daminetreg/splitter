---
chapter: splitter waltkthrough
chapter-label: 01 / Input · split
notes: The same graph as the Build Atomiticity one, with the launcher in the compile edge. The build system still sees one edge — the same inputs, the same use_mylib.o out. Inside it the splitter parses once, writes the preamble and its PCH, a rewritten mylib.h with declarations only, one source per body — add, multiply, greet, average, and the definitions piece that owns main — compiles each into its own object, and ld -r joins them into the object the build asked for. Every one of those inner compiles is an edge the build system does not know about, and the launcher runs only the ones whose input changed. The link edge is untouched.
---
## Same build, {split}parallel graph{/split}.

::: mermaid
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

Same inputs, same object out. Original TU compilation, expands to one compile per body.
