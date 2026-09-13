# 45 — remember a decline, keyed on the unit's inputs

## Motivation

A declined unit — one the splitter decided before writing anything that it cannot split
correctly (TODO/44) — is decided again on every launcher run. Nothing records the decision:
a decline writes no `split.cache`, so the next run cannot take the "inputs unchanged" path,
and it goes on to build the prefix PCH, parse the unit with libclang, walk the inclusions and
decline once more. With `CPP_SPLITTER_REMOTE_SPLIT` on, it first asks the cluster for a split
the worker then declines. Measured on OpenCV: about 5s per declined unit per rebuild
(`arithm.dispatch.cpp`, `alloc.cpp`), for a result that cannot differ unless an input does.

The decision is already *stable* — the pair header's manifest re-raises it on every run
(`dad864e3`) — but not *cached*.

## Implementation Proposal

1. When `do_split()` declines, the launcher writes `<split_dir>/declined` holding the inputs
   hash `split_inputs_hash()` would compute — which needs `depfile.cache`, so the passthrough
   compile's `cache_passthrough_depfile()` runs first and the marker is written after it —
   and the reason, as printed.
2. On the next run, before `read_split_cache()` and before `try_remote_split()`: if
   `<split_dir>/declined` exists and its hash equals the current inputs hash, print
   `declined earlier: <reason>` and go straight to the passthrough compile. No PCH, no parse,
   no round trip.
3. If the hash differs, remove the marker and evaluate the unit as new: the edit may have
   removed the static or the second inclusion, and a unit that can be split now must be.
4. A successful split removes the marker; a decline removes `split.cache`, as a failed link
   already does, so the two records never coexist.

## Acceptance Criteria

- `split.pair_header_declines` and `split.static_unnamed_type_declines` (the runner runs each
  twice) assert that the second run prints `declined earlier` and does not print
  `libclang args:` — the line the parse prints — and still produces the right program.
- A third run after an edit that removes the cause — the pair header no longer defining a
  function, the static given a named type — splits the unit, with pieces written.
- OpenCV, `one header`: the three declined units report `declined earlier` and no parse.
- With `CPP_SPLITTER_REMOTE_SPLIT`, a remembered decline reaches the cluster for nothing:
  no `rewrapper` invocation for that unit, checked in `launcher.remote_split_on_rbe`'s log.
