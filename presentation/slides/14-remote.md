---
chapter: Architecture
chapter-label: Remote Execution & Caching
notes: Section 9 — emit-only splitter is invoked using CMake RE's rewrapper when remote configuration exists; tree returns through output directories and compiles/link follow.
---
## {accent}Remote{/accent} C++ splitting 

Leverage CMake RE's rewrapper to distribute and cache splitting.

::: mermaid
flowchart LR
    subgraph local["Developer machine"]
        direction TB
        launcher["cpp-splitter"] --> rw["rewrapper<br/>action = the compile command"]
        rw --> reproxy["reproxy scans inputs"]
        tree[".split tree"] --> pieces["piece compiles and ld -r,<br/>each a remote action"]
    end
    subgraph cluster["RBE cluster"]
        worker["cpp-splitter --emit-only<br/>libclang parse"]
        cache["action cache"]
    end
    reproxy --> worker
    worker -- "-output_directories" --> tree
    pieces --> cache
    class launcher,worker,pieces split
    class rw,reproxy,cache violet
    class tree warn
:::

::: rationale
Parsing C++ requires alot of RAM depending the codebase, leverage scalable build distribution cluster with Bazel RBE when run with CMake RE, also with remote compilation and scheduling.
:::