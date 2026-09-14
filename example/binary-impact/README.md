# What splitting does to the binary

`compare.sh` builds the `use_mylib` fixture of the test suite -- one source, one header of
four `inline` functions and a function template -- five ways from the same CMake project and
compares the executables: sizes, symbols, per-function machine code, `diffoscope`. Measured
on 14 September 2026 with clang 13.0.0 (tipi toolchain `4f846ee`), `-O2`, lld as the final
linker in every configuration. TODO/46.

| configuration | compiler launcher | flags | pieces combined by |
|---|---|---|---|
| plain | — | `-O2` | — |
| plain-lto | — | `-O2 -flto=thin` | — |
| split | `cpp-splitter` | `-O2` | `ld -r` |
| split-lto-relink | `cpp-splitter` | `-O2 -flto=thin` | `ld.lld -r`: LTO over the unit's pieces, native object out |
| split-lto-final | `cpp-splitter` | `-O2 -flto=thin` | `llvm-link` (`llvm-link-r.sh`): one bitcode object out, LTO at the final link |

## Enabling LTO with the splitter

Under `-flto=thin` the pieces are bitcode, and GNU `ld -r`, the splitter's default
relocatable linker, does not read bitcode (`file format not recognized`; the unit falls
back). `CPP_SPLITTER_LINKER` names another:

- `CPP_SPLITTER_LINKER=ld.lld` -- lld runs LTO over the pieces when asked for a relocatable
  output and writes a native `.o`. Cross-piece inlining happens here, per unit; the final
  link sees ordinary objects and does no LTO across units.
- `CPP_SPLITTER_LINKER=<this directory>/llvm-link-r.sh` -- `llvm-link` merges the pieces
  into one bitcode module (the wrapper drops the `-r` the splitter passes). The final
  `-flto=thin` link optimises across the whole program, as it does for a plain LTO build.

## What was split

The unit's four `inline` header functions each get a piece
(`include/mylib.h_1_add.cpp` … `mylib.h_4_average.cpp`), emitted as `inline` with
`__attribute__((used))`; the copy of `mylib.h` every piece includes declares them. `main`
is kept (the program's entry point) in `use_mylib.cpp_0_definitions.cpp`; the template stays
in the header copy.

## Results

### Sizes and symbol counts

| configuration | executable | `.text` | `.data` | unit object | object `.text` | symbols (exe) | symbols (object) |
|---|---:|---:|---:|---:|---:|---:|---:|
| plain | 9096 | 3422 | 600 | 5008 | 1058 | 26 | 5 |
| plain-lto | 8824 | 3296 | 600 | 8848 | bitcode | 24 | 2 |
| split | 9968 | 3947 | 608 | 7416 | 1537 | 29 | 9 |
| split-lto-relink | 9672 | 3695 | 600 | 7152 | 1366 | 29 | 10 |
| split-lto-final | 9600 | 3835 | 608 | 9924 | bitcode | 29 | 5 |

Bytes; `symbols` counts `llvm-nm --defined-only`. All five programs print the same five
lines.

### Symbols

Against plain, every split executable has exactly three symbols more, and no symbol fewer:

```
W add(int, int)
W average(std::vector<double> const&)
W multiply(int, int)
```

`__attribute__((used))` keeps each piece's function in its object whether or not anything
calls it, and `inline` makes it weak. In plain these three are inlined into `main` and never
emitted; `greet` (85 instructions) is not inlined in plain either and is `W` in both. The
static-function rename does not occur here: the fixture has no `static`. The remaining
differences are the numbering of `GCC_except_table*` local symbols and, in the object, the
constant-pool labels.

### Code, function by function

`asmdiff.py` normalises what layout alone decides (addresses, jump targets, rip-relative
displacements, address-sized immediates, padding) and leaves out the PLT stubs and the C
runtime's start-up code, which differ by layout only.

| against plain | identical | differ | only in the other | instructions plain → other |
|---|---:|---|---|---:|
| split | `greet`, `add`/`multiply`… | `main` 91 → 122 | `add` (2), `multiply` (3), `average` (22) | 178 → 236 |
| plain-lto | — | `main` 91 → 157 (`greet` inlined into it) | `greet` only in plain | 178 → 159 |
| split-lto-relink | `main`, `greet` | none | `add` (2), `multiply` (3), `average` (22) | 178 → 205 |
| split-lto-final | `greet` | `main` 91 → 110 | `add` (2), `multiply` (3), `average` (22) | 178 → 224 |

**split.** `main` calls `add`, `multiply` and `average` instead of inlining them: each is
compiled in a piece of its own, and `main`'s piece sees only their declarations. The visible
cost is not the three calls: in plain, with `average` inlined, clang folds the whole of
`std::vector<double> vals = {1.0, …, 5.0}; average(vals)` to the constant `3.0` and the
vector is never allocated; in split the vector is built (`operator new`, five stores,
`operator delete`, and an unwind path) and passed to `average`. `.rodata` gains the five
`double` constants and the exception tables grow accordingly -- that is what `diffoscope`
shows beyond the code: the `.rodata` string dump, `.gcc_except_table`, `.eh_frame`, one more
PLT entry (`operator new`), and the section and dynamic-table offsets that follow.

**split-lto-relink.** `ld.lld -r` runs LTO over the unit's pieces and `main` comes out
identical to plain's, instruction for instruction: `add`, `multiply` and `average` inlined,
the vector folded away. The three `used` functions stay as weak symbols nothing calls, 27
instructions and 273 bytes of `.text` over plain. This is plain's code plus the dead
copies, not plain-lto's: nothing is inlined across units, so `greet` remains a call, as in
plain.

**split-lto-final.** The final link optimises the merged bitcode: `add` and `multiply` are
inlined into `main`, but `average` is not folded -- `main` still allocates the vector (110
instructions against plain's 91) and calls `average`. Each piece was already optimised at
`-O2` before the merge, and the LTO pipeline does not recover the fold that plain's
single-function compilation found. Compared with plain-lto (157 instructions, `greet`
inlined), it inlines less, not more.

### diffoscope

The text reports are `results/<a>-vs-<b>.diffoscope` (1400–1650 lines each). Beyond the
code and symbol differences above they list only what follows from them: section sizes and
offsets, the dynamic table, `.eh_frame`, `.gcc_except_table`, `.rodata`, `.strtab`.

## What this says

- Splitting changes the code of a unit wherever the plain compile inlined a function that
  now lives in another piece; on this fixture that is one call site, and the second-order
  effect (the constant-folded vector) is larger than the calls themselves: `.text` +15%.
- `__attribute__((used))` leaves a weak copy of every header function in the object; the
  linker does not discard them. The archive symbol counts in
  `benchmarks/opencv-local-split.md` are this, at scale.
- LTO at the relink (`ld.lld -r`) brings `main` back to plain's code exactly; the `used`
  copies remain. LTO at the final link (`llvm-link`) does not: pre-optimised pieces merge
  into something the LTO pipeline inlines less than a plain LTO build does.

## Reproducing

```sh
cmake --build build -j32                     # the splitter, from the repository root
example/binary-impact/compare.sh             # results/report.md and the per-pair reports
DIFFOSCOPE=diffoscope example/binary-impact/compare.sh   # with diffoscope's reports too
```

`results/` is not committed. `diffoscope` needs `libmagic` and `libarchive`; the run above
used diffoscope 329 from a virtualenv with both unpacked from their Ubuntu packages.
