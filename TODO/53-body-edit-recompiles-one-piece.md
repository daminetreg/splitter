# 53 — a body edit recompiles one piece: the `#line` directives of the pieces after it

## Motivation

`benchmarks/synthetic-unity.md`: a line added to the body of `unit_100_fn_20()`, the 21st
of 40 functions in its unit, is re-sliced without a parse and then compiles 20 pieces, not
one. Every piece opens with `#line <start line> "<source>"` so that diagnostics and debug
information name the source rather than the piece, and a line added to one body moves
the start line of every definition after it in the file. The re-slice rewrites those
directives (`try_incremental_split()`, the `shift` loop), the pieces' text changes, and
the launcher's up-to-date check -- the piece newer than its object -- recompiles them.
Boost.Spirit's `toucs4` is the 18th of 20 functions in `standard_wide.hpp`; on the
synthetic corpus the body row is 0.7s of which the one piece that changed is a twentieth.

## Implementation Proposal

The object of a piece whose only change is its `#line` number is byte-identical to the
one it has, unless the line number reaches the code. It reaches the code through debug
information (`-g` and its family, `-gline-tables-only`, `-ggdb`...) and through the
preprocessor: `__LINE__`, `__builtin_LINE()`, `std::source_location::current()` --
directly or through a macro (`assert`, `BOOST_ASSERT`, `BOOST_TEST`, `CHECK`...).

- **Line-sensitive definitions, decided at harvest time.** The macro definitions the
  translation unit read (`CXCursor_MacroDefinition`, tokens via `clang_tokenize`) give
  the set of macros whose replacement mentions `__LINE__` or `__builtin_LINE`, closed
  over macros that mention one of those. A definition is line-sensitive when its body
  mentions `__LINE__`, `__builtin_LINE`, `source_location`, or a line-sensitive macro
  (the preprocessing record's `MacroExpansion` cursors inside its extent, or the
  identifiers of its text). The harvest records it: a flag on the `D` line.
- **The re-slice leaves the object alone when the line cannot reach it.** For each piece
  after the edit that is not line-sensitive, when the compile flags carry no `-g`
  variant: rewrite the `#line` directive as now, then touch the object so that it stays
  newer than its source -- or, cleaner, record beside the object the hash of the piece
  with its `#line` directive masked, and have `needs_recompile()` compare that. The
  latter survives a launcher that runs twice.
- **With `-g`** the directives matter and the pieces recompile as now. A second step,
  `CPP_SPLITTER_LINES=lazy`, would keep the objects with their stale line tables and mark
  the unit "lines to settle", recompiling those pieces on the next build that touches the
  unit for another reason; not part of this TODO's acceptance.
- **Shared pieces (TODO/51)** have no `#line` directive and are untouched by this: a
  body edit in a header already costs one compile per function of that header.
- **The definitions unit and the copies** are unaffected: the edit is inside a body, the
  preamble does not change.

## Acceptance Criteria

- `launcher.line_shift_edit`: a unit of three functions built with `-O2` and no `-g`; a
  line added to the first body recompiles exactly one piece (the other two objects keep
  their timestamps, the log names one compile), the program prints the new result. The
  same edit with `-g` recompiles three. A variant whose third function calls `assert`
  recompiles two without `-g`: the edited one and the line-sensitive one.
- `benchmark-synthetic.sh`: the body row reports 1 piece compile; the time goes from
  0.7s toward the launcher's floor.
- Boost.Filesystem and Boost.Spirit: 0 fallbacks, 0 declined; the Spirit body row
  unchanged (its pieces are shared).
