# 04 — Split headers are written flat by basename and shadow each other

**Severity:** High, and the most dangerous failure mode in this list: it silently
substitutes one header for another instead of producing an error.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

Auto-split headers are all written into a single flat directory named by basename, and
that directory is placed **first** on the include path (`-I<split_dir>`) when compiling
split pieces. Two headers with the same file name from different directories therefore
collide: one overwrites the other on disk, and every `#include` of that name — from any
file — resolves to whichever one won.

Confirmed on the Boost `filesystem` build. The split directory for `directory.cpp`
contains a single `atomic_ref.hpp`, but two distinct source headers map to that name:

- `libs/filesystem/src/atomic_ref.hpp` — declares `namespace atomic_ns = boost;`
- `libs/atomic/include/boost/atomic/atomic_ref.hpp` — Boost.Atomic's public header

Boost.Atomic's copy wins (`grep -c atomic_ns` on the emitted file returns 0). So when
`atomic_tools.hpp` does `#include "atomic_ref.hpp"`, it now gets a header that never
declares `atomic_ns`, and the split pieces fail with:

```
atomic_tools.hpp:30:12: error: use of undeclared identifier 'atomic_ns'; did you mean 'atomics'?
```

### Second facet: stem collision destroys the source file's own split output

The same flat layout also collides on the *stem* used to name split pieces. A translation
unit and a header with the same base name share one split directory and one
`<stem>_<n>_<fn>.cpp` naming scheme, and the stale-output pruning at the end of
`emit_split_files()` deletes every `<stem>_*.cpp` in the directory that is not in the
current run's file list:

```cpp
if (fname.rfind(stem + "_", 0) != 0) continue;
if (std::find(current_files.begin(), current_files.end(), path) == current_files.end()) {
    fs::remove(entry.path());
```

`libs/filesystem/src/path.cpp` and `libs/filesystem/include/boost/filesystem/path.hpp`
both have stem `path` and both split into `path.cpp.o.split/`. The header split runs
second, via `resolve_header_deps()`, and prunes all 54 of `path.cpp`'s own
`path_*.cpp` files as "stale" — they are not in `path.hpp`'s list of 154. The launcher
then tries to compile the file list it was given and fails with:

```
clang++: error: no such file or directory: '.../path.cpp.o.split/path_1_find_separator.cpp'
clang++: error: no input files
```

So the primary translation unit's split output is silently destroyed by the splitting of
a same-named header. This is currently the blocker for `path.cpp` in the Boost example.

Here the substitution happens to produce a compile error. It just as easily might not:
two same-named headers with compatible-looking contents would produce a silently
miscompiled object. Boost alone has many repeated basenames (`config.hpp`,
`operations.hpp`, `exception.hpp`, `detail/header.hpp`, ...), and the flat layout also
lets a split copy shadow an unrelated original that the translation unit expected to
get from a real include directory.

## Description

The split directory is flat: `<output.o>.split/` receives `<basename>` for every split
header, alongside `<basename>.pch`, `<basename>.split` manifests, and the
`<stem>_<n>_<fn>.cpp` pieces. The launcher then compiles split pieces with

```cpp
cmd += " -I" + shell_quote(split_dir);
for (const auto& hdr_dir : sr.header_obj_dirs)
    cmd += " -I" + shell_quote(hdr_dir);
```

so `split_dir` precedes the project's real include directories. Three consequences:

1. Same-basename headers overwrite each other in the output directory.
2. The surviving copy shadows the original for *all* consumers, not just its own
   split pieces.
3. The `.split` manifest and `.pch` file names are basename-derived too, so their
   caches collide in the same way and can go stale against the wrong source header.

### Implementation plan

1. **Stop flattening.** Mirror each split header at the path it was included as, under a
   dedicated subdirectory:

   ```
   <output.o>.split/include/<path as included>/<basename>
   ```

   `clang_getInclusions()` already gives the `CXFile` for every inclusion; recover the
   spelling used at the include site, or compute the header's path relative to whichever
   `-I` directory resolved it. Writing to that relative path preserves the semantics of
   both `#include "atomic_ref.hpp"` and `#include <boost/atomic/atomic_ref.hpp>`.
2. Add `-I<split_dir>/include` in place of `-I<split_dir>`, so the rewritten tree is
   structurally interchangeable with the originals and ordering against the real
   include directories is meaningful again.
3. Key the `.pch` and `.split` manifest files by a hash of the header's **absolute
   path** (plus content hash, as today) rather than by basename, so two headers with the
   same name get distinct cache entries.
4. Add a collision assertion: while emitting, if a target output path is already claimed
   by a *different* source header, fail loudly rather than overwrite. This turns any
   remaining path-mapping gap into a visible error instead of a silent substitution.
5. If two include directories legitimately resolve the same relative path (a real
   shadowing setup in the original build), preserve the original resolution order rather
   than picking arbitrarily; record the resolved absolute path in the manifest so
   staleness checks compare like with like.
6. Audit `resolve_header_deps()` and `header_obj_dirs` for the same basename assumption.
7. Fix the stem collision: give each split source its own output subdirectory, or key the
   `<stem>_<n>_<fn>.cpp` naming and the stale-pruning filter on the source file's
   absolute path rather than on its stem. The pruning predicate must never be able to
   match another source's outputs.

## Acceptance Criteria

- A Boost `filesystem` split build emits a distinct output file for
  `libs/filesystem/src/atomic_ref.hpp` and for
  `libs/atomic/include/boost/atomic/atomic_ref.hpp`; neither overwrites the other.
- The `atomic_ns` error is gone from the build log.
- No two split headers in a build share an output path; the collision assertion never
  fires on the Boost example.
- `.pch` and `.split` manifest names are unique per source header, verified by a build
  containing at least two same-basename headers.
- Regression fixture: a project with `a/config.hpp` and `b/config.hpp`, both included by
  one translation unit with different contents, splits and compiles correctly.
- Regression fixture: `foo.cpp` that includes its own `foo.hpp`, where both contain
  splittable functions — neither one's split pieces may be pruned by the other, and both
  must still be present when the launcher compiles them.
- `libs/filesystem/src/path.cpp` retains its own split pieces after
  `boost/filesystem/path.hpp` has been split into the same build.
- Removing `-I<split_dir>` from a compile command no longer changes which original
  headers resolve (i.e. the split tree no longer shadows unrelated originals).

## Outcome

Implemented in `src/main.cpp`:

- New layout helpers `include_dirs_from_flags()`, `header_mirror_relpath()`,
  `split_include_root()` and `path_hash()`. Headers are mirrored under
  `<split_dir>/include/` at the path they were included as, resolved by longest-matching
  `-I` / `-isystem` / `-iquote` directory; a header under no include directory falls back
  to `_abs/<hash>/<basename>` so the mapping stays unique.
- Split pieces are now named `<sanitized full file name>_<n>_<fn>.cpp` instead of
  `<stem>_...`, and the stale-output pruning and `make_static_mangled_name()` key off the
  same tag, so `path.cpp` and `path.hpp` can no longer match each other's outputs.
- `claim_output_path()` fails loudly if two different sources ever map to one output path.
- `resolve_header_deps()` now computes the manifest path the same way
  `write_header_manifest()` writes it. These disagreed before (`<stem>.h.split` versus
  `<filename>.split`), so the staleness check never found a manifest and every header was
  re-split on every invocation.
- `load_header_manifests()` recurses into the mirrored tree, and `SplitHeaderInfo.split_dir`
  is the include root rather than the per-header directory, so `-I` stays a single entry.
- The mirrored include root is added to the compile commands in both launcher and CLI mode.

Verified on the Boost `filesystem` build:

| check | before | after |
|---|---|---|
| `undeclared identifier 'atomic_ns'` (basename collision) | present | **0** |
| `no such file or directory: .../*.split/...` (stem collision) | present | **0** |
| output path collisions reported by the new guard | n/a | **0** |
| translation units failing on their own split pieces | 8 | **6** |
| corrupted `_cast` tokens (TODO 02 regression check) | 0 | **0** |

`libs/filesystem/src/atomic_tools.hpp` and `libs/atomic/include/boost/atomic/atomic_ref.hpp`
now mirror to `include/atomic_tools.hpp` and `include/boost/atomic/atomic_ref.hpp`
respectively. `libs/filesystem/src/atomic_ref.hpp` contains no function definitions, so no
rewritten copy is emitted and the real header is used -- which is why `atomic_ns` resolves
again.

Still 2 of 12 translation units link from split objects, and 9 fall back. The remaining
blockers are:

- **TODO 05** — now dominant. `exception.cpp` and `path_traits.cpp` stopped failing on
  their own pieces and now fail on header dependencies instead; every remaining header-dep
  failure is a constructor or member extracted to namespace scope
  (`system_error`, `lock_guard<Mtx>`, `atomic_count`), and `path.cpp`, `directory.cpp`,
  `operations.cpp` and `codecvt_error_category.cpp` fail the same way through
  `boost/filesystem/path.hpp`.
- **TODO 03** — `unique_path.cpp`, unchanged: `use of undeclared identifier
  'fill_random_dev_random'` from the un-renamed preamble.
