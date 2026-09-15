---
chapter: Opening
chapter-label: Translation Unit (TU) Fission
notes: Before the atom, the thing it is an atom of. A build is a graph — files are the nodes, the actions that make one file from others are the edges. The build system's whole job is this graph — run an edge when one of its inputs is newer than its output, skip it otherwise, and the cache and the cluster key on the same edges. Here is ours for use_mylib.cpp — the fixture the rest of the talk splits. One compile edge — every header the unit includes goes in, one object comes out. One link edge. The compile edge is the atom — nothing inside it is visible to the build system, the cache or the scheduler. Touch anything on its input side and the whole edge reruns.
---
## A build is a {accent}graph{/accent}.

::: mermaid
flowchart LR
    h["📄 mylib.h"] --> c
    s["📄 use_mylib.cpp"] --> c
    sys["📚 &lt;string&gt; &lt;vector&gt;<br/>&lt;numeric&gt; &lt;algorithm&gt;"] --> c
    c(["⚙️ compile TU<br/>clang++ -c use_mylib.cpp"]) --> o["🧱 use_mylib.o"]
    o --> l(["🔗 link<br/>clang++ -o use_mylib"])
    l --> exe["🚀 use_mylib"]
:::

The build system reruns when a file changes; The atoms of builds are {violet}TUs{/violet} (Translation Units).

It's the finest-granular action that can be taken in a C++ build.