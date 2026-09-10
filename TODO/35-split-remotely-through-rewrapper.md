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

### `-inputs`: let reproxy's dependency scanner compute them

A split needs exactly what compiling that translation unit needs — the source and its include
closure. Nothing more, since `--emit-only` neither compiles nor links. So the right move is not
to compute that list ourselves but to ask for the same scanning an ordinary C++ compile gets:

```
-labels=type=compile,compiler=clang,lang=cpp
```

Those labels select reproxy's `CPPInputProcessor`, which runs the bundled `scandeps_server`
over the command line and produces the transitive header set. It is the same machinery
`tipi-compiler-driver` already relies on for every piece compile in the build, so the inputs a
split declares come from the same source of truth as the inputs its pieces declare.

**The obstacle is argv[0].** The processor expects to be scanning a *compiler* invocation, and
our command begins with `cpp-splitter`. The way out is `-remote_wrapper`: hand rewrapper the
plain compile line — `<compiler> <flags> -c -o <obj> <source>` — so the scanner sees a genuine
clang command, and let the wrapper put the splitter in front of it on the worker:

```
rewrapper -labels=type=compile,compiler=clang,lang=cpp \
          -exec_strategy=remote \
          -remote_wrapper=<abs path to cpp-splitter, with --emit-only> \
          -toolchain_inputs=<abs path to cpp-splitter> \
          -output_directories=<split_dir> \
          -- <compiler> <flags> -c -o <obj> <source>
```

This is the first thing to prove, because the whole approach rests on it. Two questions to
settle by experiment before writing anything else:

1. Does the CPP input processor accept the command line when a `-remote_wrapper` is present, or
   does the wrapper have to be part of the scanned command?
2. Does `-remote_wrapper` take arguments of its own (`--emit-only`), or only a program path? If
   only a path, the mode has to be selected by environment instead — `-env_var_allowlist` is
   the knob for getting a variable to the worker.

**Fallback if the processor will not scan a wrapped command.** Declare the closure ourselves:
run `<compiler> -M -MG <flags> <source>`, parse the make rule, and pass it via
`-input_list_paths` with `-labels=type=tool`, the label that does no scanning and takes the
caller's list. A `-M` run is a preprocess and far cheaper than the parse it replaces, but it is
a second dependency computation that can disagree with the one reclient does for the pieces —
which is exactly the class of bug this project keeps finding. Prefer the scanner.

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

### Test it against the cluster, not against a stub

A stub `rewrapper` would only prove we format a command line the way we intended. It cannot
show that the inputs we declared were sufficient, that the outputs came back, or that the
sandbox reproduced what a local split produces — and those are the three things that can
actually be wrong. The test runs against `opal.cluster.engflow.com:443`.

`launcher.remote_split_on_opal` — a CTest test over a deliberately tiny project, two
translation units behind one shared header, configured with `cmake-re --distributed` and
`CPP_SPLITTER_REMOTE_SPLIT=1`.

**Gating.** It needs mTLS credentials and a reachable cluster, so it must not fail on a machine
that has neither. Report `SKIP_RETURN_CODE` when `$ENGFLOW_MTLS_DIR` (default `~/engflow-mTLS`)
holds no readable certificate and key, or when the endpoint does not answer. Skipped and passing
are different states and the log must say which.

**What it asserts, in order of how much each is worth:**

1. **The split tree came back byte-identical** to `cpp-splitter --emit-only` run locally on the
   same sources. This is the assertion that matters: it covers the declared inputs being
   sufficient, `-output_directories` bringing everything home, and the sandbox not perturbing
   the absolute paths baked into the preamble and the `#line` directives.
2. **The split really executed remotely.** `RBE_proxy_log_dir` is already set by
   `build-spirit-cmake-re.sh`; read the records back with
   `reclient/<rev>/dumpstats --proxy_log_dir=<dir>` and require a `REMOTE_EXECUTION` for the
   action whose command names `cpp-splitter`. Without this the test passes just as well when the
   feature silently fell back to splitting here, which is precisely the failure mode to catch.
3. **No fallbacks**, and the linked programs print what a plain build's do.

**Corroboration from the cluster's own side.** cmake-re prints an `Invocation ID`; the profile
is downloadable with the same mTLS key at
`https://${RBE_service}/api/profiling/v1/instances/default/invocations/<id>` and opens in
Perfetto. The split actions should appear there as remote work. Worth doing by hand when the
numbers look wrong; not worth asserting on in a test, since it adds a second network dependency
to prove something `dumpstats` already proves locally.

Then the harness ladder: `example/boost-to-split` with `BOOST_INCLUDE_LIBRARIES=filesystem`
first, and only then Spirit through `example/spirit-tests/SpiritTestsFromJamfiles.cmake`.

## Acceptance Criteria

- The CPP input processor scans the command and returns the same header closure a piece compile
  gets — demonstrated before any of the rest is built, since the design rests on it.
- `cpp-splitter --emit-only` writes the same split tree as a normal run and compiles nothing:
  byte-identical `.split` contents against a run without the flag, checked on a
  Boost.Filesystem unit.
- `launcher.remote_split_on_opal` passes against the real cluster, and skips — visibly, not
  silently — where there are no credentials. It must fail when `CPP_SPLITTER_REMOTE_SPLIT` is
  unset, and it must fail when the split ran locally, which is what the `REMOTE_EXECUTION`
  record on the `cpp-splitter` action is there to detect.
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

## Outcome

Implemented and verified against the real cluster.

`remote_split_enabled()` gates the whole path on `CPP_SPLITTER_REMOTE_SPLIT`, and
`remote_split_env()` reads `RBE_server_address`, `RBE_exec_root` and `RBE_platform` from the
environment reproxy already exports, so the launcher borrows cmake-re's proxy rather than
starting one. `try_remote_split()` runs before any parsing: it stages the `cpp-splitter` binary
by content hash under `<cwd>/.cpp-splitter/<hash>/`, invokes `rewrapper` with the compile
command as the action, and reads the returned tree through `read_split_cache()`. With
`CPP_SPLITTER_EMIT_ONLY=1` in `-env_var_allowlist`, the copy on the worker writes the split
tree and exits before compiling anything.

Four details cost a full debugging round each and are worth keeping written down:

- `-remote_wrapper` is resolved **relative to the working directory**, while
  `-toolchain_inputs` is relative to the **exec root**. The same path in both makes the worker
  die inside `execvp()`.
- The binary has to live under the exec root to be uploaded at all, hence the staging copy.
- EngFlow refuses an action with no platform: `-platform=container-image=docker://…@sha256:…`
  is required, not optional.
- `-labels=type=compile,compiler=clang,lang=cpp` is what makes reproxy's CPP input processor
  scan the command and upload the header closure. Nothing else declares the inputs, and it
  works through `-remote_wrapper`.

Against the acceptance criteria:

- **Dependency scanning through the labels** — confirmed before writing any of this, with a
  probe whose only declared input was the wrapper script; reproxy found the header anyway.
- **`--emit-only` is byte-identical and compiles nothing** — confirmed on a Boost.Filesystem
  unit.
- **`launcher.remote_split_on_opal`** — passes. It builds a two-unit project through
  `cmake-re --host --distributed`, requires the `remote split: N piece(s) returned from the
  cluster` line, requires reclient's records to show the action served by the cluster and **no**
  local execution or fallback, then rebuilds the same sources with the feature off and compares
  the two `.split` trees byte for byte. It skips only when there are no mTLS credentials or no
  cmake-re, and it says so.
- **Boost.Filesystem, 0 fallbacks** — a local split build of `BOOST_INCLUDE_LIBRARIES=filesystem`
  is clean.
- **Boost.Spirit's suite, 0 fallbacks, produced on the cluster** — the full ported suite built
  through `cmake-re --host --distributed -j64` with the feature on: 549 edges, 268 programs,
  **279 units split remotely, 0 fallbacks**, 279 `REMOTE_EXECUTION` records and 14.1 GB of split
  trees downloaded.

Three criteria are measurements rather than behaviour and are **not** yet recorded: the `full`
and `one body` rows against the numbers in `benchmarks/boost-spirit-rbe-summary-9-Sep-2026.md`,
and peak local RSS at `-j500`. The benchmark has to be re-run to settle them.

Two mistakes made while testing this, both of which produced a *passing* build that proved
nothing, and which the test now guards against explicitly:

- `find_program(cmake_re ... PATHS ...)` appends to the default search, so it found the
  v0.0.87 cmake-re on `PATH` instead of the v0.0.88 in `cmake-re-dev-latest/`. Only from
  v0.0.88 does cmake-re chain a user-supplied `CMAKE_CXX_COMPILER_LAUNCHER` ahead of
  `tipi-compiler-driver`; the older one drops it silently, so the splitter never ran. The test
  now reads `CMAKE_CXX_COMPILER_LAUNCHER` back out of the cache and fails if cpp-splitter is
  not in it.
- Removing the work directory does not force a cold build: `-B` is a symlink into cmake-re's
  mirror and cmake-re keys the directory it points at on the configuration, so the next run
  adopts whatever was configured there before. The test resolves the link and removes the real
  directory.
