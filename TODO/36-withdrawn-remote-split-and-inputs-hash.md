# 36 — a remote split leaves no `inputs.hash` — **withdrawn, the premise was a bad measurement**

## Motivation

Filed on the belief that TODO/28 and TODO/35 were individually correct and jointly useless: a
split produced on the cluster appeared to leave no prerequisite record behind, so the next body
edit could not tell which prerequisite had changed, could not re-slice, and went back to the
cluster for a full split of every unit that included the edited header. The `one body` row of
`benchmark-spirit-cmake-re.sh` did behave exactly that way — 9420 remote actions and 805.5s
against 1 and 75.5s for a split produced locally.

The evidence offered was:

```
$ find <split build dir> -name '*.harvest'  | wc -l      # 88569
$ find <split build dir> -name inputs.hash  | wc -l      # 112, against 279 split directories
```

**That count was taken while the build was still running.** It is a snapshot of a tree being
rewritten, not a finished state, and it does not support the conclusion drawn from it. The
first thing to check was whether the file was absent *after* the build, and that was not done.

## What is actually true

The prerequisite list survives a remote split, and the mechanism is worth writing down because
it is not obvious from either feature:

`emit_split_files()` attaches `-MD -MF <the unit's depfile>` to the unit's **first piece
compile** — and the pieces are compiled here even when the split was not. So the compiler
writes the unit's depfile exactly as it always did, `rewrite_depfile()` turns it into
`depfile.cache`, and `write_inputs_hashes()` records what every prerequisite hashed to. None of
that code knows or cares where the split came from.

A worker-side writer for `depfile.cache`, built from the include closure the parse already
walks, was implemented and then reverted: it is redundant, since the local piece compile
overwrites the file moments later, and it existed only to satisfy a diagnosis that was wrong.

The real cause of the `one body` row was the defect fixed in *do not merge, and do not list
every piece twice*: `read_split_cache()` appended to the `SplitResult` handed to it, so once a
candidate cache was read and rejected before `try_remote_split()` read the returned split into
the same struct, every piece appeared on the `ld -r` command line twice. `ld` reported every
symbol as multiply defined, 265 of 279 units fell back to a plain compile, and a unit that
falls back leaves no harvest — so the run after it had nothing to re-slice from and asked the
cluster for a full split. One bug, wearing the costume of a design problem.

## Outcome

No code change. The invariant is now a test, which is the part worth keeping:
`launcher.remote_split_on_opal` gained a phase that splits on the cluster into an empty tree,
checks that `inputs.hash` and `depfile.cache` are there for every split directory, edits one
function body, and requires the rebuild to re-slice locally and **not** to reach the cluster.
It clears `.o.d` files along with the pieces, because a leftover depfile from the local phase
above supplies that same list and made an earlier version of this phase pass whether or not the
thing it tested was true.

The lesson for the next entry like this one: a `find | wc -l` against a live build tree is not
a measurement, and an interaction between two features is the least likely explanation to reach
for before the last commit has been ruled out.
