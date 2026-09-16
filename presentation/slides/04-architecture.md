---
chapter: Architecture
notes: UML
---

## Architecture

::: mermaid
classDiagram
    direction LR

    class Launcher["Launcher"] {
        decides: reuse · re-slice · remote · parse
        compiles the pieces, ld -r into the object
    }
    class DriverProbes["Driver probes"] {
        what libclang is given so it parses the compiler's program
    }
    class Parse["Parse"] {
        one libclang parse per unit, the include prefix precompiled
    }
    class Harvest["Harvest"] {
        every definition's extent, linkage, scope chain, references
    }
    class Classification["Classification"] {
        per definition: piece, kept in the preamble, or definitions header
    }
    class TextRewriting["Text rewriting"] {
        pure functions over source text; no libclang
    }
    class Emit["Emit"] {
        preamble · definitions header · pieces · .harvest · .keeps
    }
    class Headers["Headers: candidates and mirror"] {
        which included files to split, and where their copies go
    }
    class CompileAndPCH["Compile and PCH"] {
        the preamble PCH, the pieces in parallel, ld -r
    }
    class CacheAndReslice["Cache and re-slice"] {
        split.cache · inputs.hash · depfile.cache · modules.hash
    }
    class RemoteSplit["Remote split"] {
        rewrapper: emit-only on a worker, the tree comes back
    }
    class FilesAndPaths["Files and paths"] {
        read, hash, name, quote
    }


    Launcher --> DriverProbes
    Launcher --> CacheAndReslice
    Launcher --> RemoteSplit
    Launcher --> Parse
    Launcher --> CompileAndPCH
    Parse --> DriverProbes
    Parse --> Harvest
    Parse --> Headers
    Parse --> Emit
    Emit --> Classification
    Emit --> TextRewriting
    Headers --> Emit
    Classification --> TextRewriting
    CacheAndReslice --> TextRewriting
    Emit ..> FilesAndPaths
    Headers ..> FilesAndPaths
    CacheAndReslice ..> FilesAndPaths
    cssClass "Launcher,Parse,Harvest,Classification,Emit" split
    cssClass "TextRewriting,Headers,CacheAndReslice" accent
    cssClass "DriverProbes,CompileAndPCH" violet
    cssClass "RemoteSplit" warn
    cssClass "FilesAndPaths" muted
:::