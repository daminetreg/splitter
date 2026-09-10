# 35 — Produce the split files on the cluster, through rewrapper

## Motivation

A distributed split build sends every piece to the cluster and does the splitting here. Measured
on Boost.Spirit's 277 test programs (`benchmarks/boost-spirit-rbe-summary-9-Sep-2026.md`), the
local half is what is left to win:

| | wall | remote actions |
|---|---:|---:|
| `full`, plain, all cached | 32.1s | 0 |
| `full`, split, all cached | 456.0s | 0 |

Neither of those compiled anything. The 456s is bookkeeping and *local work*: 277 libclang
parses, 4848 pieces written, and a `ld -r` per unit. On one machine the same build is 654.5s
against 64.2s, so the parse and emit are the bulk of what splitting costs and the only part a
farm currently cannot help with.

The splitter is also the one action in the graph whose peak memory is large — it holds a full
translation unit AST — which is what makes high `-j` values a question here and not for an
ordinary compile.

Moving parse-and-emit to the cluster would put every expensive part of a split build on
machines that are not this one.

## Implementation Proposal

### The shape: emit-only, not the whole unit

The remote action must stop after writing the pieces. If it also compiled and linked them, the
unit would become **one** action, and a one-line body edit would invalidate all of it — which
is exactly the win being protected: today that edit executes 1 remote action against an
ordinary build's 271. Keep the piece compiles as separate actions driven from here.

So: a new `--emit-only` mode that runs `do_split()` and stops, then the existing local path
picks up from the returned `SplitResult`.

### Where it hooks in

`run_as_launcher()`, immediately after the arguments are parsed and before `do_split()` at
`src/main.cpp:5109` — the split is either performed here or fetched from the cluster, and
everything downstream is unchanged. Gate it on `chained_behind_driver` (already computed at
5083): the splitter only knows a cluster exists when cmake-re has put `tipi-compiler-driver` in
front of it. Add `CPP_SPLITTER_REMOTE_SPLIT=1` to opt in while this is experimental, and an
`_IN_REMOTE_SPLIT` marker in the child's environment so the remote invocation cannot recurse.

### `-inputs`: the guess is right, and there are two ways to satisfy it

A split needs exactly what compiling that translation unit needs — the source and its include
closure. Nothing more, since `--emit-only` neither compiles nor links.

**Baseline: declare them ourselves.** Run `<compiler> -M -MG <flags> <source>` first, parse the
make rule, and hand the list to rewrapper via `-input_list_paths=<file>` with
`-labels=type=tool` (the label that does no scanning and takes the caller's list). A `-M` run is
a preprocess: far cheaper than the parse it replaces, and the only new local cost. Note there is
no `-M` path in the tool today — `detect_system_includes()` and `probe_driver_standard()` are
the only preprocessor invocations and both run against `/dev/null`.

**Optimisation to try second: let reproxy discover them.** `-labels=type=compile` makes reproxy
run its own dependency scanner (`scandeps_server`) over a *compiler* command line, which is
what the C++ path normally uses instead of an explicit list. Our command is `cpp-splitter`, so
the action would have to be presented as a compile of the source with the real compiler and the
splitter injected by `-remote_wrapper` (the flag exists; its exact semantics are unverified).
This removes the `-M` run entirely. Do not build on it until it is demonstrated.

### `-output_directories`: also right, and necessary

Piece names cannot be known before the parse — they are `<tag>_<counter>_<name>.cpp` where both
the counter and the name come from the AST walk, and most counter slots produce no file at all
(11515 candidates against 273 files on one Boost.Geometry unit). `split.cache` lists them, but
only from the previous run, and they move whenever a function is added or removed.

`split_dir` however is deterministic: `fs::absolute(output_file) + ".split"`
(`src/main.cpp:5059`). So declare `-output_directories=<split_dir>` and download the lot. The
flag exists and takes paths relative to `-exec_root`.

### Four hazards that need answers, not assumptions

1. **The split dir is an input as well as an output.** `header_split_candidates()` skips a
   header whose manifest is newer than the header (`src/main.cpp:3441`) — an *mtime* test
   against files in the output tree, which a sandbox will not reproduce. Either declare the
   split dir in `-inputs` too, or move that staleness test onto content, for which
   `inputs.hash` and `<tag>.harvest` already exist.
2. **The stale-output pruner deletes what it did not produce** (`src/main.cpp:4248`). Remotely
   the directory starts empty so pruning is inert, but the downloaded tree must then *replace*
   the local one rather than merge into it, or pieces from a previous shape survive.
3. **The toolchain has to be there.** `-toolchain_inputs=<path to cpp-splitter>` pulls a listed
   executable's runtime dependencies, which is how `libclang.so` and the pinned clang reach the
   worker. The environment image is already pinned by digest
   (`environments/ubuntu-clang.pkr.js`).
4. **Paths.** cmake-re mirrors sources under `$TIPI_HOME/vT.w/<hash>-<project>`; rewrapper wants
   everything relative to `-exec_root`. The mirror root is the natural exec root. Absolute paths
   baked into the preamble and the `#line` directives must survive the round trip — they name the
   mirror, which exists on both sides, so this should hold, and it is the first thing to check
   when output differs.

### Unit test: assert the contract, not the cluster

A fixture cannot depend on an RBE cluster, and it does not need to. What is risky here is *what
we tell the execution engine*; that is checkable locally.

`launcher.remote_split_contract` — put a stub `rewrapper` first on `PATH` that appends its argv
to a file and then execs the command it was given, so the split still happens and the object
still links. Then assert on the recorded argv:

- `-labels=type=tool` is present;
- `-output_directories` names exactly `<object>.o.split`;
- `-input_list_paths` names a file that contains the source **and** the header it includes;
- `-toolchain_inputs` names the `cpp-splitter` binary;
- the program the split object produces still prints the expected value.

The stub is the same technique as `launcher.chained_behind_driver`, which uses an `exec "$@"`
stub to stand in for `tipi-compiler-driver`. It discriminates: with the feature off, no
`rewrapper` invocation is recorded at all.

Then, per the harness ladder: `example/boost-to-split` with `BOOST_INCLUDE_LIBRARIES=filesystem`
first, and only then Spirit through `example/spirit-tests/SpiritTestsFromJamfiles.cmake`.

## Acceptance Criteria

- `cpp-splitter --emit-only` writes the same split tree as a normal run and compiles nothing:
  byte-identical `.split` contents against a run without the flag, checked on a
  Boost.Filesystem unit.
- `launcher.remote_split_contract` passes, and fails when `CPP_SPLITTER_REMOTE_SPLIT` is unset —
  a fixture that cannot fail is worse than none.
- With the feature enabled and a real cluster, Boost.Filesystem builds through
  `cmake-re --distributed` with **0 fallbacks** and the library passes its nine assertions.
- Boost.Spirit's 277 programs build with **0 fallbacks**, and the split `.split` trees are
  byte-identical to those from a local split of the same sources.
- The `one body` row keeps its shape: **1** remote compile against the ordinary build's 271. If
  it rises toward 271, the action granularity has been lost and the design is wrong.
- The `full` split row improves against the 456.0s recorded for an all-cached distributed build,
  since the parses no longer run here. Report it with reclient's per-action counts, because a
  wall time alone cannot distinguish work moved from work cached.
- Peak local RSS during a `-j500` split build falls measurably, which is the second reason for
  doing this at all.
