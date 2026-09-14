---
chapter: Architecture
chapter-label: 09 / Remote Execution & Caching
notes: Section 9 — emit-only splitter is invoked using CMake RE's rewrapper when remote configuration exists; tree returns through output directories and compiles/link follow.
---
## {accent}Remote{/accent} C++ splitting 

Leverage CMake RE's rewrapper to distribute and cache splitting.

::: flow
- CPP_SPLITTER_REMOTE_SPLIT=1 cpp-splitter  | 
- rewrapper cpp-splitter\n(local preprocessor input scan) | violet
- remote worker\nlibclang parse | split
- download split tree | teal
:::

::: rationale
Parsing C++ requires alot of RAM depending the codebase, leverage scalable build distribution cluster with Bazel RBE when run with CMake RE, also with remote compilation and scheduling.
:::