# Boost.Filesystem: `ld` vs `mold` vs `lld` for the relocatable link — 5 September 2026

Measured at `ad90bca`. Companion to `boost-filesystem-bench-5-Sep-2026.md`, which covers the
split build against a plain one; this one holds the splitter fixed and varies only the
linker.

## Why measure this at all

Splitting turns one link of twelve objects into twelve links of several hundred. A step that
is a rounding error in a normal build becomes something worth choosing deliberately, and the
three linkers available here disagree by a factor of two on speed and thirty percent on
output size.

`CPP_SPLITTER_LINKER` selects the program. It defaults to `ld` and is passed `-r` either way,
since GNU ld, mold and lld all spell relocatable output the same, so any of them is a drop-in.

## The linkers

| | version |
|---|---|
| `ld` | GNU Binutils, the default |
| `mold` | 2.30.0 |
| `ld.lld` | 13.0.0, from the tipi clang `4f846ee` toolchain |

`ld.lld` is not on the default `PATH` in this container; it lives in the toolchain's `bin`.

## The link step in isolation

The largest translation unit's split output, 72 objects, linked five times and averaged:

| linker | per link | output | vs `ld` |
|---|---:|---:|---|
| `ld` | 41 ms | 11M | — |
| `ld.lld` | 29 ms | 7.9M | 1.4x faster, 28% smaller |
| **`mold`** | **20 ms** | **7.7M** | **2.1x faster, 30% smaller** |

mold is the fastest and produces the smallest object; lld is between the two and much closer
to mold than to `ld`.

## End to end

The full benchmark, run once per linker, everything else identical:

| scenario | `ld` | `mold` | `ld.lld` | plain |
|---|---:|---:|---:|---:|
| full | 7.3s | 7.4s | 7.4s | 0.7s |
| no-op | 0.2s | 0.2s | 0.2s | 0.2s |
| one source | 0.2s | 0.2s | 0.2s | 0.5s |
| one header | 0.3s | 0.3s | 0.3s | 0.7s |
| one function body | 1.8s | 1.8s | 1.8s | 0.7s |
| **library** | 12M | **9.3M** | 9.5M | 2.2M |
| build tree | 269M | 265M | 265M | 6.5M |

**The timings are indistinguishable.** Every wall-clock difference is inside the run-to-run
variation measured for these scenarios, which is around 2%.

## Analysis

### Speed: real, but not where the time is

mold really is twice as fast at this step, and the isolated numbers are not noise — five
repetitions, a consistent gap. It does not show up end to end because the link is roughly 5%
of a split build. Halving 5% is 2.5%, which is inside the noise floor of the measurement.

Concretely: twelve units, saving about 21 ms each, is a quarter of a second out of 7.4.

This is the sort of result that is easy to report either dishonestly optimistic — quoting
only the 2.1x — or dishonestly dismissive — quoting only the end-to-end table and concluding
the linker is irrelevant. Both single numbers are true and neither is the finding. The
finding is that the link is not the bottleneck, and that changing it therefore cannot help
much *yet*.

### Size: the result that actually matters here

`ld` produces a library 29% larger than either alternative, and this is not a rounding error
on an artefact that is already six times larger than the unsplit one.

The split library is large because every piece carries its own copy of whatever inline and
template code it uses, and a relocatable link is where those copies could be merged. mold and
lld evidently merge more of that duplication than GNU ld does. That the two LLVM-adjacent
linkers land within 2% of each other, and `ld` sits 29% above both, suggests this is a
difference in what they do rather than an accident of one version.

### When this would change

The link's share grows if the compile side gets faster or if units split into more pieces.
`TODO/14` records that the remaining incremental cost is now the split itself rather than
compilation, and if that were addressed the link would become a visible fraction. A project
splitting into thousands of pieces per unit rather than hundreds would also shift the
balance, since link cost scales with object count more directly than compilation does.

## Recommendation

**Use mold when it is available**, on size rather than speed: 22% off the library for a
one-word environment variable, with no measurable time cost and no correctness difference.
lld is an equally good second choice and is already present wherever a clang toolchain is.

Keep `ld` as the default. It is the one linker that is certainly installed, the difference
does not currently affect build time, and a benchmark harness that silently depends on an
optional tool is worse than one that is 2.5% slower.

## Correctness

Each linker was checked, not assumed. For all three:

- 12 of 12 translation units split, no fallbacks;
- the launcher used the requested linker for all 12 relocatable links;
- the library links a program with no undefined references and passes the same nine
  Boost.Filesystem assertions as a library built without the splitter.

One measurement was discarded along the way. An early lld build reported a 20M library,
against 9.5M from the benchmark harness for the same linker. The difference was the build
directory, not the linker: the harness configures from scratch, the ad-hoc build had been
reused. The numbers above all come from the harness, which configures cleanly for every
measurement.

## Reproducing

```sh
CPP_SPLITTER_LINKER=mold   ./benchmark-boost-split.sh
CPP_SPLITTER_LINKER=ld.lld PATH=/usr/local/share/.tipi/clang/4f846ee/bin:$PATH \
    ./benchmark-boost-split.sh
```

For the isolated link timing, point a loop at one unit's `.o` files:

```sh
D=<build>/libs/filesystem/CMakeFiles/boost_filesystem.dir/src/operations.cpp.o.split
OBJS=$(find $D -name '*.o')
time $LINKER -r -o /tmp/out.o $OBJS
```
