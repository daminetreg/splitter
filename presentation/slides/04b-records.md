---
chapter: Architecture
chapter-label: Parse and re-slicing
notes: depfile.cache because :   1. To hand the build system a depfile when no compile wrote one. CMake passes -MD -MF use_mylib.o.d and expects the file to exist after the command. 2. As the prerequisite list for the content hashes. split_inputs_hash() and write_inputs_hashes() read depfile.cache to know which files to hash: the source, its flags, and every header the compiler actually read.
---
## Avoid reparsing  
to change {split}only a body{/split}

::: cards 3
- depfile.cache |  | Original prerequisites remain visible to the build.
- inputs.hash |  | Content hashes for prerequisites.
- harvest |  | Function body locations and their body/signature hashes
:::

::: rationale
Record content and source geometry (extents) so a body-only change can recreate the same split tree without rebuilding the AST: re-hash + split + rebuild only the changed body.
:::