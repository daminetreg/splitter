# 25 — Two defects behind Boost.Geometry's cold-build fallback

**Severity:** High. The second is a silent correctness hole, not merely a lost split: a unit
can drop every definition a header contributed and still report success.

Both were found by disproving `TODO/24`'s claim that Boost.Test's `progress_monitor.ipp` is
unsplittable. It is not. Building the same target twice is enough to see it:

| run | fallbacks | units split |
|---|---:|---:|
| cold | **1** (`area.cpp` itself) | 50 |
| warm, after `touch area.cpp` | **0** | 51 |

The cold failure is defect 1. The warm "success" is defect 2 hiding it.

## Defect 1 — a definition needing a retracted macro is moved out anyway

`boost/test/impl/progress_monitor.ipp`:

```cpp
#define PM_SCOPED_COLOR() \
    BOOST_TEST_SCOPE_SETCOLOR( s_pm_impl().m_color_output, ... )
...
void progress_monitor_t::test_start( counter_t test_cases_amount, test_unit_id )
{
    ...
    PM_SCOPED_COLOR();
    ...
}
...
#undef PM_SCOPED_COLOR
```

`undefined_macros()` already knows about this shape -- `TODO/16` added it, so a body needing a
macro the file retracts is kept in the preamble rather than moved into a piece, where the
`#undef` would already have run. It sets `keep_in_header`, and `keep_reason()` reports
"needs a macro the file undefines".

What it does not do is reach the *other* placement decision. `generate_preamble()` then looks
at the same definition, sees external linkage and no `inline`, and moves it into the
definitions header — which begins by including the preamble, by which point the macro is gone:

```
progress_monitor.ipp_definitions.h:19:5: error: use of undeclared identifier 'PM_SCOPED_COLOR'
```

This is the same omission as the macro-fragment case fixed a day earlier: a guard that decides
*whether to split* has to be consulted again when deciding *where to put what was kept*.

### Why "keep it in the preamble" is not obviously enough

The preamble is included by every piece, so a strong definition left there is defined once per
piece and `ld -r` rejects the copies. That is exactly what the definitions header exists to
avoid, and it is why the earlier analysis concluded no placement works.

The missing option is the one the splitter already uses everywhere else in a header: emit it
**`inline`**. A definition split out of a header is written `inline` with
`__attribute__((used))` for precisely this reason -- several objects may carry it and the
linker merges them. A definition *kept* in a header's preamble can have the same treatment.
It is well defined even for a function with a local `static`: all copies share one object.

## Defect 2 — a header split on a previous run contributes no pieces

`header_split_candidates()` skips a header whose manifest is newer than the header:

```cpp
if (fs::exists(manifest) && !stale) continue;
```

Correct as far as it goes -- there is no need to split it again. But the skip is a bare
`continue`, so the header never enters `g_split_headers`, and `resolve_header_deps()` builds
`header_obj_files` **only** from that map:

```cpp
for (const auto& inc_path : includes) {
    auto it = g_split_headers.find(inc_path);
    if (it != g_split_headers.end()) { ... result.header_obj_files.push_back(obj); }
}
```

So the pieces of an already-split header are neither compiled nor linked. The unit reports
success having silently dropped every definition they carry.

`load_header_manifests()` exists and does exactly the right thing -- reads the manifests from
disk and populates `g_split_headers` -- but it is called from the command-line driver only,
never from the launcher.

### Why this has not broken everything

Each translation unit splits into its own directory, so a manifest exists only if *this same
object* was built before. On a clean tree everything is split in process and the map is
complete. On a rebuild, `split.cache` normally short-circuits the whole split and returns the
previous `SplitResult`, `header_obj_files` included.

The hole opens when the cache is absent but the manifests are not: the previous run **fell
back**, which returns before `write_split_cache()`. That is precisely the Geometry sequence --
fall back, then rebuild -- and it is why the warm build "succeeded". It links because nothing
in that program referenced the dropped symbols. A program that did would fail to link, and the
report would point at the library rather than at the splitter.

## Reproduction

```sh
cmake -GNinja -S example/boost-to-split -B /tmp/geo-cold \
    -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake \
    -DBOOST_INCLUDE_LIBRARIES=geometry -DBUILD_TESTING=ON \
    -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
CPP_SPLITTER_VERBOSE=1 ninja -C /tmp/geo-cold -j8 boost_geometry_algorithms_area   # 1 fallback
touch example/boost-to-split/libs/geometry/test/algorithms/area/area.cpp
CPP_SPLITTER_VERBOSE=1 ninja -C /tmp/geo-cold -j8 boost_geometry_algorithms_area   # 0 fallbacks
```

For defect 2 on its own, after the second run:

```sh
d=$(find /tmp/geo-cold -path '*area.cpp.o.split*' -name progress_monitor.ipp | xargs dirname)
sed -n 2p $d/progress_monitor.ipp.split      # 4 compilable pieces listed
ls $d/progress_monitor.ipp_2_operator+=.o    # missing: never compiled, never linked
```

## Implementation spec

1. **Consult the retracted-macro rule at the placement decision.** `generate_preamble()` moves
   a kept definition into the definitions header when `definitions && r.fn &&
   !has_vague_linkage(*r.fn)`. Add `&& !r.fn->uses_undefined_macro`. The flag is already on
   `FunctionInfo` and already set by `prepare_functions()`.

2. **Emit it `inline` where it stays.** A definition kept in the preamble for this reason would
   otherwise be a strong symbol in a header every piece includes. Insert `inline` using
   `inline_insertion_point()`, which already knows not to put it in front of a template's
   parameter list and to place it after a `template <>` prefix. Do nothing if the text already
   says `inline`.

3. **Register a skipped header's manifest.** Where `header_split_candidates()` skips a header
   whose manifest is current, read that manifest into `g_split_headers` instead of returning
   nothing. `load_header_manifests()` already parses this format; factor the per-file half of
   it out and call it from both places rather than duplicating the parse.

4. **Do not pre-load every manifest instead.** Calling `load_header_manifests()` up front looks
   simpler and is wrong: `header_split_candidates()` tests `g_split_headers` *before* it tests
   staleness, so a pre-loaded stale header would be skipped rather than re-split, and header
   edits would stop being detected. The registration has to happen on the not-stale path only.

## Acceptance Criteria

- `boost_geometry_algorithms_area` splits on a **cold** build, with no fallback.
- A regression fixture: a header that defines a macro, uses it in an out-of-line member
  definition with external linkage, and undefines it -- split, linked and run.
- A regression fixture for defect 2: one object built twice, where the first run leaves header
  manifests without a split cache, and the second run's object still defines every symbol the
  header's pieces carry. `nm` on the object, not just a successful link, because the whole
  point is that it links either way.
- The four-library harness still splits with no fallbacks, and the filesystem example still
  scores 9/9.

## Outcome

**Defects 1 and 2 are fixed. A third, found on the way, is not — and the reason it is not is
the useful part of this file.**

### Defect 1 — fixed

`generate_preamble()` no longer moves a definition into the definitions header when
`uses_undefined_macro` is set, and emits it `inline` where it stays so the copy every piece
gets merges. `PM_SCOPED_COLOR` no longer appears in any error: `grep -c` on the build log is
zero where it used to be the first failure.

### Defect 2 — fixed, and it was worse than the fallback it was hiding

A header skipped because its manifest is current now registers that manifest in
`g_split_headers`, so its pieces are compiled and linked. Measured on the same object:

| | pieces with a `.o` | warm build |
|---|---|---|
| before | 0 of 4 | reported success |
| after | **4 of 4** | falls back, honestly |

That reads like a regression and is the opposite of one. The warm build never legitimately
succeeded: it linked an object missing every definition those pieces carried, and got away
with it only because that program referenced none of them. A program that did would have
failed to link, and the report would have pointed at Boost.Test rather than at the splitter.

### Defect 3 — conversion operators are harvested now, and the program still misbehaves

With defect 1 cleared, the cold build's next failure was:

```
multiple definition of `boost::test_tools::tt_detail::context_frame::operator bool()'
```

`visitor()` harvested `FunctionDecl`, `CXXMethod`, `Constructor`, `Destructor` and
`FunctionTemplate` -- but not `ConversionFunction`, although `collect_emitted()` has always
counted them. An unharvested definition is never moved out of the preamble, so this one was
copied into every piece and `ld -r` rejected the copies.

`CXCursor_ConversionFunction` is in that list now, on `main`, and the cold build splits with no
fallbacks at all -- the whole Boost.Geometry test suite included. The program it produces
**fails at run time**:

```
$ boost_geometry_algorithms_area          # plain
$ echo $?  -> 0
$ boost_geometry_algorithms_area          # split
Test setup error: There is no argument provided for parameter color_output
$ echo $?  -> 200
```

Bisected: it is not defect 1, not defect 2, and not the emission of pieces for conversion
operators -- keeping them in the header and merely *relocating* them to the definitions header
reproduces it. The relocated text is textually correct:

```cpp
namespace boost { namespace test_tools { namespace tt_detail {
context_frame::operator bool()
{
    return true;
}
```

The cause is still not understood. `example/conversion-operator/` is the Boost-free repro to
iterate on. `is_conversion` on `FunctionInfo` forces `keep_in_header`, so the harvest cannot
also start emitting pieces -- `operator T()` names its type where a return type would go, and
the out-of-line form rebuilt from `return_type + qualified_name` comes out as
`bool C::operator bool()`.

A fourth thing fell out of trying: `sanitize_filename()` produced a 419-character component from
`operator typename base_type::value_type()`, past `NAME_MAX`, after which every filesystem call
on that path threw an uncaught `filesystem_error` and the splitter aborted instead of falling
back. The component is capped now.

### Where Boost.Geometry stands

Its tests no longer fall back at all -- cold or warm, all six benchmarked targets split, which
is what `benchmarks/boost-geometry-bench-7-Sep-2026.md` measures on 8 September. What remains
is defect 3's run-time symptom: the objects are produced without giving up, and the program
they make still exits 200 where the plain one exits 0. Building is not passing, and this file
is the only place that says so.
