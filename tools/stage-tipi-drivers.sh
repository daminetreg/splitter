#!/usr/bin/env bash
#
# Make the tipi driver binaries runnable, and print the directory to prepend to PATH.
#
# cmake-re drives compiles, links, ar and ranlib through helpers under $TIPI_HOME/<driver>/<rev>/,
# referenced by bare name and found on PATH. In tipibuild/tipi-ubuntu-2404:v0.0.87 every one of
# them is mode `-rwxrw-r--` owned by the `tipi` user: the owner may execute them, the group may
# not. The container recipe this repository documents runs as your own uid with `--group-add
# tipi`, so the group bits are the ones that apply and every driver fails with
#
#     /bin/sh: 1: tipi-compiler-driver: Permission denied
#
# Nothing here can fix the mode -- the files belong to another user and we are not root. What it
# can do is take copies we own, make those executable, and hand back a directory to put first on
# PATH. Drivers that are already runnable are left alone, so this whole thing becomes a no-op
# once the image ships them group-executable.
#
# Usage:  PATH="$(tools/stage-tipi-drivers.sh /somewhere/drivers [tipi-home]):$PATH"
#
# Prints the directory to prepend, or nothing when every driver was already runnable.
set -euo pipefail

dest="${1:?usage: stage-tipi-drivers.sh <destination directory>}"
home="${2:-${TIPI_HOME_DIR:-/usr/local/share/.tipi}}"

staged=0
for src in $(find "$home" -maxdepth 3 -type f -name 'tipi-*-driver' 2>/dev/null); do
    name="$(basename "$src")"
    [ -x "$src" ] && continue          # already runnable as us
    if [ ! -r "$src" ]; then
        echo "cannot read $src, and it is not executable either" >&2
        exit 1
    fi
    mkdir -p "$dest"
    if [ ! -x "$dest/$name" ] || [ "$src" -nt "$dest/$name" ]; then
        cp "$src" "$dest/$name"
        chmod u+x "$dest/$name"
    fi
    staged=$((staged + 1))
done

[ "$staged" -gt 0 ] && echo "$dest" || echo ""
