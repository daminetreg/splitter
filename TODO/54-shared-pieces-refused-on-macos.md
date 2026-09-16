# 54 — every shared piece is refused on macOS: `nm` shows a weak definition as `T`

## Motivation

TODO/51's store shares one object per header function across every unit that includes
the header, on one condition: the object defines nothing that may exist once only. A
header can hold a non-inline definition — an implementation include's members, a
namespace-scope variable — and the compiler then emits it strong; an object shared by every
includer would put that symbol in each of them. `store_object_has_strong_symbols()` asks the
compiler's verdict rather than predicting it: `nm -g --defined-only`, `T D B R C` strong,
`W V` weak, and a strong symbol sends the unit back to its own per-unit piece with

```
[cpp-splitter] shared piece defines a symbol that may exist once only, the unit compiles its own: …/mylib.h
```

That is what every macOS build prints, for every header, `example/mylib/mylib.h` included,
whose functions are all `inline`. The check reads ELF conventions into Mach-O. GNU `nm`
prints a `linkonce_odr` definition as `W`; on Mach-O a weak definition is an ordinary `T`
carrying the `weak_definition` attribute, which only `nm -m` shows:

```
$ nm -g --defined-only w.o           # an inline add() anchored with `used`
0000000000000000 T __Z3addii
$ nm -m w.o
0000000000000000 (__TEXT,__text) weak external __Z3addii
```

So on macOS the store compiles every shared piece, refuses every one, and the unit compiles
its own: the pre-TODO/51 build, plus one wasted compile per header function. The build is
correct; nothing is shared. `launcher.shared_header_piece` says so — it fails on the
`macos-brew-llvm` and `macos-apple-clang` builds with "the store holds 0 object(s) for
shared_fn()" — and has been failing there since the store landed, which nobody noticed
because the Linux job is the one that runs it in CI.

## Implementation Proposal

Read the weak flag where Mach-O keeps it, keep the ELF parse where it works.

- On Darwin (`__APPLE__`), run `nm -m -g --defined-only` and parse its form: a line is weak
  when it says `weak external` (or `weak private external`), strong when it is `external`
  in a `__TEXT`/`__DATA`/`__const`/`__common` section without `weak`. Undefined and local
  symbols are not printed with `-g --defined-only`.
- Or one parser for both: `llvm-objdump --syms` marks weak with `w` in the flags column on
  every format, and Homebrew's and Apple's toolchains both ship it beside the compiler; a
  toolchain without it falls back to today's `nm` parse. `llvm-nm` alone does not help — it
  prints Mach-O weak definitions as `T` as well.
- The tool is resolved next to the compiler first (`which_on_path()` as for the linker
  driver), so the build's own binutils are asked, not whatever `nm` is first on PATH.
- No `nm` and no `objdump`: keep refusing, as today (`return true`), and say so once in the
  verbose log rather than per piece.

## Tests

- `launcher.shared_header_piece` passes on the three macOS builds as it does on Linux; it
  already asserts that the store holds exactly one object for `shared_fn()` and that the
  second unit links it without compiling.
- A new fixture `launcher.shared_piece_refused_for_strong_symbol`: a header with an inline
  function *and* a namespace-scope `int counter = 0;` — the shared compile succeeds, the
  object carries `counter` strong, the store must refuse it and the unit compile its own
  piece, on both platforms. Today the refusal is right on macOS for the wrong reason, so the
  fixture is what keeps the fix from over-correcting into sharing everything.
- The example builds in CLAUDE.md's order: Boost.Filesystem with 0 fallbacks on the
  `macos-brew-llvm` build, then the Spirit suite through
  `example/spirit-tests/SpiritTestsFromJamfiles.cmake`; the piece counts in
  `benchmarks/` for the shared store should then hold on macOS as they do on Linux.

## Outcome (16 September 2026)

`store_object_has_strong_symbols()` reads `nm -m -g --defined-only` under `__APPLE__`: a
defined external symbol is weak when its line says `weak`, strong otherwise (`external` in
`__TEXT`, `__DATA`, `__common`); the ELF parse is unchanged. Unit tests only, no Boost build
this time, and no Linux run (Docker was down); the ELF branch is byte-for-byte the old one.

- `launcher.shared_header_piece` passes on `macos-brew-llvm`, where it failed before with
  "the store holds 0 object(s) for shared_fn()".
- `launcher.shared_piece_refused_for_strong_symbol` (`test/shared_piece_strong/`): `once.h`
  holds a non-inline `strong_once()` beside an inline `once_fn()`; the shared piece for
  `once_fn()` carries `strong_once` strong and is refused with a `.fail` marker while
  `shared_fn()` from `shared.h` is shared once. A non-inline header function on its own is
  never a store candidate, which is why the fixture pairs the two. Passes on macOS;
  63/63 on the `macos-brew-llvm` build.
- Not done: the CI matrix change and the `example/mylib` store check on the other two macOS
  builds.

## Acceptance Criteria

- `launcher.shared_header_piece` passes on `macos-brew-llvm`, `macos-apple-clang` and
  `macos-clang`, and the CI matrix runs it on the macOS job.
- `launcher.shared_piece_refused_for_strong_symbol` passes on macOS and Linux, and fails
  on a build that treats every object as weak.
- A macOS split of `example/mylib` logs no "shared piece defines a symbol that may exist
  once only" for `mylib.h`, and `.cpp-splitter-store` holds one object per function of it.
