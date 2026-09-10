# 36 — a remote split leaves no `inputs.hash`, so the next edit cannot re-slice

## Motivation

TODO/35 moves the split to the cluster. TODO/28 makes a one-line body edit cost one re-slice
instead of a parse. Measured together on Boost.Spirit's suite at `-j500`, the second does not
happen after the first: the `one body` row with the split produced on the cluster re-parses
every unit that includes the edited header, where the same row with the split produced locally
re-slices and executes a single compile.

The cause is not the order the three paths are tried in — that was fixed separately, and
`try_incremental_split()` is now attempted first. It is that the path cannot run at all,
because the file it needs is missing:

```
$ find <split build dir> -name '*.harvest' | wc -l
88569
$ find <split build dir> -name inputs.hash | wc -l
112          # against 279 split directories
```

`try_incremental_split()` calls `changed_prerequisites()`, which compares the current content
hash of every prerequisite against `inputs.hash`. `write_inputs_hashes()` returns immediately
when `depfile.cache` is absent, and `depfile.cache` is written by `rewrite_depfile()` from the
compiler's `-MF` output. In the remote case there is no such output on this machine: the unit
was never compiled here, only split, so the launcher has no prerequisite list to hash and
writes nothing. The next edit therefore knows only that *something* changed, which is not
enough to re-slice, and falls through to a full split — on the cluster, for every affected
unit.

So the two features are individually correct and jointly useless, which is the worst of the
three possible outcomes and the reason this is worth its own entry rather than a line in
TODO/35.

## Implementation Proposal

The splitter already knows the include closure of the unit: it walks it with
`clang_getInclusions()` during the parse, and the parse is exactly what happened on the worker.
So the worker should write `depfile.cache` itself, into the split directory it is already
returning.

1. In emit-only mode, after the split succeeds, write `depfile.cache` from the closure the
   parse produced, in the same format `rewrite_depfile()` writes it — `<output>: <space
   separated prerequisites>`. It lands in `<split_dir>`, which `-output_directories` already
   brings home, so no change is needed to the rewrapper command line.
2. On the local side, when `try_remote_split()` succeeds, call `write_inputs_hashes()` for that
   unit. It will now find `depfile.cache` and record what every prerequisite hashed to. The
   existing call sits behind `rewrite_depfile()` and must not be moved ahead of it for the
   ordinary path, so this is a second call site rather than a reordering.
3. Confirm the prerequisite paths are usable here. The worker sees the mirror under the exec
   root, and `-platform=…,InputRootAbsolutePath=/usr/local/share/.tipi/vT.w` makes that the
   same absolute path this machine uses, which is why the returned split trees are already
   byte-identical. If any path proves worker-relative, rewrite it on arrival rather than
   teaching every reader about two path spaces.

## Acceptance Criteria

- A CTest fixture, `launcher.remote_split_then_body_edit`, extending the `remote_split` probe
  project: build it with the remote split, edit a function body in the shared header, build
  again, and require the second build to report the re-slice and **not** to have gone to the
  cluster. It must fail before the change — the point is the interaction, so a fixture that
  passes without it tests nothing.
- `inputs.hash` exists in every split directory after a distributed split build of
  Boost.Filesystem, and its prerequisite paths resolve on this machine.
- Boost.Spirit's suite, `one body` row, split on the cluster: **1** remote compile, matching
  the row where the split is produced locally, against the 4836 measured before TODO/35's
  ordering fix and the number recorded in the benchmark for after it.
- No change to any row where the split is produced locally.
