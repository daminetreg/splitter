---
chapter: Binary evidence
chapter-label: Binary / section equality map
layout: section-map
notes: Click a section. 18 sections are byte-identical and 10 differ. Dynamic metadata can be loaded, so do not say every loaded section is identical.
---
## {accent}18 equal.{/accent}  
10 metadata differences.

A schematic diff of measured section status — not a raw byte dump.

::: section-map
[
 {"name":".text","equal":true,"detail":".text is byte-identical at the same address in this measured pair."},
 {"name":".plt","equal":true,"detail":".plt is byte-identical at the same address."},
 {"name":".rodata","equal":true,"detail":".rodata is byte-identical at the same address."},
 {"name":".eh_frame","equal":true,"detail":"Unwind metadata is byte-identical."},
 {"name":".eh_frame_hdr","equal":true,"detail":"The unwind header is byte-identical."},
 {"name":".gcc_except_table","equal":true,"detail":"The exception table is byte-identical."},
 {"name":".init","equal":true,"detail":"The initialization section is byte-identical."},
 {"name":".fini","equal":true,"detail":"The finalization section is byte-identical."},
 {"name":".init_array","equal":true,"detail":"The initialization array is byte-identical."},
 {"name":".fini_array","equal":true,"detail":"The finalization array is byte-identical."},
 {"name":".got","equal":true,"detail":"The global offset table is byte-identical."},
 {"name":".got.plt","equal":true,"detail":".got.plt is byte-identical at the same address."},
 {"name":".dynamic","equal":true,"detail":"Dynamic table bytes are equal."},
 {"name":".interp","equal":true,"detail":"The interpreter section is byte-identical."},
 {"name":".note.ABI-tag","equal":true,"detail":"The ABI note is byte-identical."},
 {"name":".tm_clone_table","equal":true,"detail":"The clone table is byte-identical."},
 {"name":".comment","equal":true,"detail":"The comment section is byte-identical."},
 {"name":".shstrtab","equal":true,"detail":"The section-name table is byte-identical."},
 {"name":".symtab","equal":false,"detail":"Five split source-file names and local symbol numbering add 184 B."},
 {"name":".strtab","equal":false,"detail":"String-table metadata differs with source-file names."},
 {"name":".dynsym","equal":false,"detail":"Dynamic symbol ordering follows the relinked metadata."},
 {"name":".dynstr","equal":false,"detail":"Dynamic string ordering follows the relinked metadata."},
 {"name":".hash","equal":false,"detail":"Hash metadata changes with dynamic-symbol ordering."},
 {"name":".gnu.hash","equal":false,"detail":"GNU hash metadata changes with dynamic-symbol ordering."},
 {"name":".gnu.version","equal":false,"detail":"Version metadata follows dynamic-symbol ordering."},
 {"name":".gnu.version_r","equal":false,"detail":"Version requirement metadata follows symbol ordering."},
 {"name":".rela.dyn","equal":false,"detail":"Relocation metadata differs in ordering; imports are the same."},
 {"name":".rela.plt","equal":false,"detail":"PLT relocations have the same slots but a different order."}
]
:::

::: tiny
Equal at the same addresses: {accent}.text{/accent}, {accent}.plt{/accent}, {accent}.rodata{/accent}, {accent}.got.plt{/accent}, and unwind sections. Differing dynamic metadata includes .dynsym and .rela.*; same 19 imports and PLT slots, different order.
:::