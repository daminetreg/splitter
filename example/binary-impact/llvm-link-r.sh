#!/usr/bin/env bash
# A relocatable "linker" for the splitter that keeps LTO for the final link: the pieces are
# bitcode under -flto=thin, and llvm-link merges them into one bitcode module, which is what
# the final link then optimises across the whole program. The splitter invokes its linker as
# `<linker> -r -o <out> <pieces...>`; llvm-link takes no -r.
# Named through CPP_SPLITTER_LINKER; LLVM_LINK names the llvm-link to use.
set -euo pipefail
args=()
for a in "$@"; do
    [ "$a" = "-r" ] && continue
    args+=("$a")
done
exec "${LLVM_LINK:-llvm-link}" "${args[@]}"
