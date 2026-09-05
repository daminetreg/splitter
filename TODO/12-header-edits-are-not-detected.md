# 12 — Editing a header no longer rebuilds anything that includes it

**Severity:** Critical. It is a silent correctness failure in the build, not a slowdown: the
build reports success while linking objects compiled against the previous version of the
header. Every other item in this list costs performance or fails loudly; this one hands back
a stale binary.

**Status: implemented.** See "Outcome" at the end.

## Motivation

The splitter rewrites each header it splits into a copy under
`<output>.split/include/<path as included>`, and puts that tree ahead of the project's own
include directories. Every split piece therefore includes the *rewritten* header, never the
original. The dependency file the compiler produces records what it actually opened, so the
original never appears in it, and the build system has no reason to believe the object
depends on it.

Measured on the Boost example, for `path.cpp.o`:

| | count |
|---|---|
| total dependencies recorded | 620 |
| entries that are rewritten copies under `.split/include/` | 46 |
| entries that are original project headers | 317 |
| entries naming the original `boost/filesystem/path.hpp` | **0** |
| entries naming the rewritten `path.hpp` | 1 |

Headers that were *not* split still appear as originals, which is why the depfile looks
plausible at a glance. It is precisely the split ones -- the interesting ones -- that go
missing.

## Reproduction

Against a settled build of the Boost example:

```sh
cd /tmp/boost-split-repro
CPP_SPLITTER_NO_SERVER=1 ninja          # settle

echo '// probe' >> .../libs/filesystem/include/boost/filesystem/path.hpp
CPP_SPLITTER_NO_SERVER=1 ninja
```

Nothing that includes `path.hpp` is rebuilt. Not `path.cpp.o`, not `operations.cpp.o`, not
`directory.cpp.o` -- and `path.hpp` is included by all of them. The library is relinked from
stale objects and the build reports success.

The same command on a build configured without `CMAKE_CXX_COMPILER_LAUNCHER` rebuilds ten
objects, which is the correct behaviour to compare against.

## Description

Two mechanisms have to agree for an incremental build to be correct here, and neither
currently does.

**The depfile names the wrong files.** `-MD -MF` is passed to the first split piece only,
and that piece sees the rewritten tree. So the recorded prerequisites are the rewritten
copies, which live inside the build directory and change only when the splitter regenerates
them -- which happens only when something already decided to re-run the splitter. The
dependency chain is circular and never fires.

**The splitter's own staleness check is not consulted by the build system.** The `.split`
manifest records a header's absolute path and is compared against its timestamp, so a rerun
of the splitter *would* notice the edit. But the splitter only runs when ninja decides the
object is out of date, and ninja decides that from the depfile above.

### Implementation plan

1. Record the original headers as prerequisites, not the rewritten copies. The splitter
   knows both paths for every header it rewrites -- `header_mirror_relpath()` computes one
   from the other -- so the mapping is already in hand.
2. Post-process the depfile rather than trying to make the compiler emit the right thing.
   After the split pieces are compiled, rewrite `<output>.o.d`, replacing every path under
   `<output>.split/include/` with the original it was generated from, and adding the
   original for any rewritten header the compiler opened. The launcher already owns this
   file, since it passes `-MF` itself.
3. Keep the rewritten copies in the depfile as well as the originals, not instead of them.
   A stale rewritten header should also trigger a rebuild, and listing both is harmless --
   ninja treats a missing prerequisite as dirty, which is the safe direction.
4. Add the preamble and the definitions header to the prerequisite list explicitly. They are
   generated files that every piece includes, and they change when the source changes.
5. Check the interaction with `-MT`: the target name must stay the object ninja knows about,
   which the launcher already substitutes correctly.
6. Consider having the launcher fail loudly rather than silently succeed if the depfile it
   was asked to write does not exist after compilation, since that is the state in which
   this failure mode hides.

## Acceptance Criteria

- Appending a comment to `boost/filesystem/path.hpp` rebuilds every translation unit that
  includes it, matching what a build without the launcher rebuilds.
- The set of objects rebuilt after touching any given header is identical with and without
  `CMAKE_CXX_COMPILER_LAUNCHER` set.
- `ninja -t deps` for a split object names the original headers.
- Editing a header so that it changes behaviour -- for example altering a constant returned
  by an inline function -- produces a binary that reflects the change; today it does not.
- A second `ninja` immediately after the first reports no work, both before and after such
  an edit. See TODO 13: that is not currently true even without an edit.
- Regression fixture in `test/`, registered with `add_test`, that splits a source including
  a header, edits the header's observable behaviour, rebuilds, and asserts the new result.

## Outcome

Two defects, not one. The second was invisible until the first was fixed, and on its own it
would have made the fix useless.

**The dependency file named generated files.** `rewrite_depfile()` now rewrites it after the
pieces are compiled. The original behind each rewritten header comes from the `.split`
manifests, whose first line is the absolute path the copy was generated from, and the
original source is added explicitly -- it was missing too. Generated paths are kept as well
as the originals: a stale copy should also force a rebuild, and a prerequisite that no
longer exists makes the target dirty, which is the safe direction.

**The dependency file was written only when something recompiled.** Only the first piece is
given `-MD`, so an incremental run in which that piece is already up to date produced no
dependency file at all. ninja records dependencies in its own database and takes a missing
file as "no dependencies", so the second build of a settled tree discarded everything the
first had learned -- `#deps 667` became `#deps 0`. Any edit after that was invisible again.
The rewritten file is now kept beside the pieces as `depfile.cache` and restored when
nothing regenerated it.

Verified on the Boost example:

| check | before | after |
|---|---|---|
| dependencies recorded for `path.cpp.o` | 620 | **667** |
| the original `boost/filesystem/path.hpp` among them | 0 | **1** |
| dependencies surviving a second build | 0 | **667** |
| objects rebuilt after editing `path.hpp` | **0** | **7** |
| objects rebuilt by a build without the launcher | 7 | 7 |

The last two rows are the point: the split build and the unsplit build now rebuild the same
seven translation units.

`test/depfile_main.cpp` with `test/depfile_header.hpp` and
`test/cmake/RunDepfileTest.cmake` is the regression test, registered as
`launcher.depfile_names_originals`. It asserts on the dependency file itself, which is the
contract with the build system, rather than on a simulated incremental build -- and it
first checks that the header really was split, so it cannot quietly stop covering the case
it was written for. Confirmed to fail against the previous binary and pass against this one.
