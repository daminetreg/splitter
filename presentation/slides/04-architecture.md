---
chapter: Architecture
notes: UML
---

## Architecture

::: mermaid
classDiagram
    direction TB

    class Launcher["Launcher (§1, §8)"] {
        decides: reuse · re-slice · remote · parse
        compiles the pieces, ld -r into the object
    }
    class DriverProbes["Driver probes (§2)"] {
        what libclang is given so it parses the compiler's program
    }
    class Parse["Parse (§2)"] {
        one libclang parse per unit, the include prefix precompiled
    }
    class Harvest["Harvest (§2)"] {
        every definition's extent, linkage, scope chain, references
    }
    class Classification["Classification (§3)"] {
        per definition: piece, kept in the preamble, or definitions header
    }
    class TextRewriting["Text rewriting (§3, §6)"] {
        pure functions over source text; no libclang
    }
    class Emit["Emit (§4, §5)"] {
        preamble · definitions header · pieces · .harvest · .keeps
    }
    class Headers["Headers: candidates and mirror (§5, §6)"] {
        which included files to split, and where their copies go
    }
    class Modules["C++20 modules (TODO/43)"] {
        interface unit → interface + implementation units; BMI published on change
    }
    class CompileAndPCH["Compile and PCH (§4, §8)"] {
        the preamble PCH, the pieces in parallel, ld -r
    }
    class CacheAndReslice["Cache and re-slice (§7)"] {
        split.cache · inputs.hash · depfile.cache · modules.hash
    }
    class RemoteSplit["Remote split (§9)"] {
        rewrapper: emit-only on a worker, the tree comes back
    }
    class FilesAndPaths["Files and paths"] {
    }

    class FunctionInfo {
        name · qualified_name · usr · signature
        start_offset · end_offset · body
        scope_chain · member_decl · conditionals
        is_inlined · is_template · is_static · is_virtual …
        keep_in_header
    }
    class VariableInfo {
        name · text · usr · scope_chain
        inline_in_place · anchors_users · replacement
    }
    class SplitResult {
        preamble_filename
        compilable_files
        header_obj_files · header_obj_dirs
        context_preambles
        success
    }
    class HarvestDef {
        start · end · body_open
        start_line · end_line
        body_hash · prefix_hash
        piece · piece_body_off
    }
    class ModuleUnit {
        interface · implementation
        name · gmf
    }

    Launcher --> DriverProbes : g_compiler, probes lazily
    Launcher --> CacheAndReslice : reuse? re-slice?
    Launcher --> RemoteSplit : or the cluster
    Launcher --> Parse : or parse here
    Launcher --> Modules : detect, expand @modmap
    Launcher --> CompileAndPCH : PCH, pieces, link
    Parse --> DriverProbes : build_clang_flags
    Parse --> Harvest : clang_visitChildren
    Parse --> Headers : candidates, mirror
    Parse --> Emit : split_unit
    Parse --> Modules : blank_module_syntax
    Harvest --> FunctionInfo : produces
    Harvest --> VariableInfo : produces
    Emit --> Classification : prepare_functions
    Emit --> TextRewriting : declarations, bodies
    Emit --> HarvestDef : writes .harvest
    Emit --> SplitResult : returns
    Emit --> Modules : g_module_unit, g_unit_imports
    Headers --> Emit : split_unit per header
    Classification --> TextRewriting : blank_code_noise …
    CacheAndReslice --> HarvestDef : reads .harvest
    CacheAndReslice --> TextRewriting : patch_file_once
    Modules --> ModuleUnit : produces
    RemoteSplit --> SplitResult : from the downloaded tree
    Emit ..> FilesAndPaths
    Headers ..> FilesAndPaths
    CacheAndReslice ..> FilesAndPaths
:::