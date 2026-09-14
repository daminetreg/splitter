# 46 — what splitting does to the binary

## Motivation

Every measurement so far is of build time. Nothing says what the split does to the program
that comes out: the pieces are compiled one function per translation unit, header
definitions are emitted `inline` with `__attribute__((used))`, `static` functions are renamed
and given external linkage, and the pieces are combined with `ld -r`. Each of those can
change the code, the symbol table, or the size of the object, and the archive symbol counts
in `benchmarks/opencv-local-split.md` (5231 external symbols split against 4118 plain in
`libopencv_core.a`) say that at least the symbol table changes. Whether the *code* changes,
and whether link-time optimisation recovers what one-function-per-unit compilation loses to
inlining, has not been measured.

## Implementation Proposal

A project tree, `example/binary-impact/`, around the `use_mylib` fixture of the test suite --
one source, one header of inline functions and a function template -- built by one script in
five configurations from the same CMake project, Release:

| configuration | compiler launcher | flags | relocatable link |
|---|---|---|---|
| plain | none | `-O2` | — |
| plain + LTO | none | `-O2 -flto=thin` | — |
| split | `cpp-splitter` | `-O2` | `ld -r` |
| split + LTO at the relink | `cpp-splitter` | `-O2 -flto=thin` | `ld.lld -r`, which runs LTO over the pieces and writes a native object |
| split + LTO at the final link | `cpp-splitter` | `-O2 -flto=thin` | `llvm-link`, which merges the bitcode pieces into one bitcode object the final link optimises |

GNU `ld -r` does not read bitcode, so a split build under `-flto` needs one of the two
relocatable linkers above, named through `CPP_SPLITTER_LINKER`. The `llvm-link` form takes a
wrapper that drops the `-r` the splitter passes.

The script then compares every configuration against plain, and the two LTO forms of the
split against plain + LTO, by:

- size of the executable and of the unit's object (`size`, `.text` / `.data` / `.rodata`);
- symbols: `nm --defined-only` of the object and of the executable, sets and differences,
  with the binding (`T`/`W`/`t`) of each;
- code: per-function disassembly (`objdump -d --no-show-raw-insn`, addresses stripped),
  diffed function by function so the report says which functions differ and how many
  instructions each has;
- `diffoscope` over the executables, its text report kept beside the others;
- the program's output, which has to be identical everywhere.

Results in `example/binary-impact/README.md`; the raw reports under `results/` are not
committed.

## Acceptance Criteria

- `example/binary-impact/compare.sh` builds the five configurations and writes the report
  from one invocation; every program prints the same five lines.
- The README states, per configuration, the sizes, the symbol differences with their cause
  (`inline` + `used`, the static rename), and which functions' code differs from plain.
- Whether either LTO form brings the split binary back to the plain one, and to which of
  the two plain binaries, is stated with the numbers.

## Outcome

`example/binary-impact/`: the project, `compare.sh`, `asmdiff.py`, `llvm-link-r.sh`, and the
results in its README. In short: a split changes the code of a unit wherever plain
compilation inlined a function now in another piece (`main` 91 → 122 instructions, `.text`
+15%, the constant-folded vector materialised); `__attribute__((used))` leaves a weak copy
of every header function in the object (3 symbols more, none fewer); `ld.lld -r` under
`-flto=thin` brings `main` back to plain's exact code with the copies still present;
`llvm-link` with LTO at the final link inlines less than a plain LTO build. Program output
identical in all configurations.

Added afterwards: three configurations with `-ffunction-sections -fdata-sections` and
`--gc-sections`. The copies are discarded only when nothing calls them: `split-gc` keeps them
(`main` calls them), `split-lto-relink-gc` is plain-gc's code, `.text` size and symbol table
to the byte.
