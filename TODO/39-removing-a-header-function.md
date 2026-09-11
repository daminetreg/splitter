# 39 — removing a function from a header: what it costs, and whether to keep its declaration

Design only. Nothing here is implemented.

## Motivation

The benchmarks measure one shape of edit: a body changed inside a definition that stays where
it is. TODO/28 and TODO/36 make that cost one re-slice and one compile. A second common shape is
not covered: a function definition **removed** from a header.

Today, removing a definition from a header `H` included by `N` units costs, per unit:

1. The build system re-runs the launcher, because `H` is a prerequisite and its content
   changed.
2. `split_inputs_hash()` misses. `try_incremental_split()` finds exactly one changed
   prerequisite, but no hypothesis of the form "one body changed inside definition *k*" matches
   a deleted extent, so it refuses — `the edited definition no longer fits the file` or `the
   change is not confined to one definition`, depending on the whitespace around the deletion.
3. A full split: a libclang parse here, or a `rewrapper` round trip with
   `CPP_SPLITTER_REMOTE_SPLIT`.
4. The unit's rewritten copy of `H` under `<split>/include/` is regenerated without the removed
   function — without its declaration if the unit emitted a piece for it, without its
   definition if the unit kept it. Every piece of the unit includes the unit's preamble, which
   includes that copy, so **every piece of the unit has a changed input** and is recompiled,
   and the preamble `.gch` is rebuilt.
5. Pieces of `H` after the removed definition are renumbered (`#line`) and recompile on that
   account as well.

So `N` parses and `N × pieces` compiles, against `N` compiles for a plain build. It has not
been measured. On Boost.Spirit, `standard_wide.hpp` is included by 194 units with several
pieces each, so the order of magnitude is thousands of actions where a plain build executes
194.

## The proposal under consideration

Leave the removed function's **declaration** in the rewritten copy of `H`, so the copy's bytes
do not change, no piece's inputs change, and only the removed piece (if any) and the `ld -r`
run.

### Why it is rejected as a general mechanism

1. **Errors move from compile time to link time.** A call to the removed function anywhere in
   the unit compiles against the stale declaration and fails at the final link with an
   undefined reference to a function that no longer exists in any source. Today it fails in
   the compile of the piece that makes the call, naming the file and line. A build that fails
   later and less specifically is a worse build, and the developer edit most likely to follow a
   removal — removing the calls — is exactly the one this makes harder to locate.

2. **The output becomes a function of history rather than of the inputs.** A rewritten copy
   that retains declarations for functions the source no longer has cannot be reproduced from
   the source: a `--clean` build, another machine, or the cluster producing the split
   (TODO/35) all write the copy *without* the declaration. The same source then yields two
   different split trees, and every piece's action key differs between them. The
   content-addressed cache stays correct — keys are on content — but hits across machines and
   across clean and incremental builds are lost, and `launcher.incremental_body_edit`'s
   invariant that a fast path produces byte-for-byte what a full split produces no longer
   holds by construction.

3. **The recompile is deferred, not avoided.** The next edit the fast path cannot handle
   forces a full split, which regenerates the copy without the declaration, and every piece
   recompiles then. The cost is paid once either way; retention only changes when, and adds the
   history-dependence above in the meantime.

A variant — retain only until the next full split, and record the retained declarations in
the harvest — keeps (1) and (2) and only formalises (3).

### What is not in question

A removed definition changes the interface every includer sees. A plain build recompiles all
`N` units for that reason, and it is right to. The split build's pieces including a changed
copy are the same fact at finer grain. The cost that is *not* inherent is the parse (or the
round trip) per unit in step 3, and that is the bounded win available here.

## Implementation Proposal

Extend the incremental path with a second recognised edit shape, alongside "one body
changed": **one recorded definition removed**.

1. In `try_incremental_split()`, when the one-body hypotheses all fail, test one more: for each
   recorded definition *k*, hypothesise that its extent `[start, end)` plus the whitespace up
   to the next non-blank line was deleted, and require every gap and every other definition's
   extent to hash as recorded, shifted by that delta after *k*. The harvest already carries
   what this needs: extents, gap hashes, extent hashes. Refuse if no *k* matches or more than
   one does.
2. On a match, without parsing:
   - delete the piece for *k* if one was emitted, and its object;
   - rewrite the unit's copy of the file: remove the declaration (emitted case) or the
     definition (kept case) at the copy's corresponding position, located as TODO/36 locates a
     kept body — by the unchanged text around it and the recorded hashes;
   - renumber `#line` and the source-line comment in every piece after *k*, as the body path
     does;
   - drop *k* from the harvest and from `.keeps`, merge its two neighbouring gaps, and
     re-record the harvest against the file as it now is;
   - if the deleted definition was the one the definitions header carried (an
     external-linkage kept definition), rewrite that header too.
3. The pieces then recompile as their changed inputs require — that part is unchanged and is
   the point of the analysis above — and the unit relinks.
4. Add a `one removal` scenario to `benchmark-spirit-cmake-re.sh` and
   `benchmark-spirit-tests.sh`: delete an inline function from a widely included header that
   no unit in the suite calls, so the build stays correct. Report it in the same table as the
   other rows. Measure before implementing (1)–(3), so the parse's share of the row is known.

The byte-identity test is the acceptance test, as for TODO/28: a removal handled by the fast
path must produce, file for file, what a full split of the same removal produces.

## Acceptance Criteria

- `launcher.incremental_body_edit` gains a phase that removes a function from the header —
  once one the unit emits, once one it keeps — and requires the fast path to run and its
  output to be byte-identical to a forced full split of the same removal. Both phases must
  fail without the change.
- The `one removal` benchmark row exists, and its splitter log reports the removal handled
  without a parse for every affected unit.
- The plain build's `one removal` row and the split build's are reported side by side with
  their action counts. This entry predicts the split build executes more actions on that row
  than the plain one and says so; the criterion is that no parse and no cluster round trip
  remain in it, not that it wins.
- No change to the `one body` rows.
