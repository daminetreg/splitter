# 31 — A rewritten header's quoted include resolves against the mirror, where its sibling is not

**Severity:** High, and higher than a lost split usually is. The unit falls back, and a unit
that falls back has no split cache, so it pays the whole split *and* the whole compile on every
build for ever. On the Spirit test benchmark that one unit turns a row that would read
7x faster into one that reads 2.55x slower.

## Motivation

`./benchmark-spirit-tests.sh` reports one fallback, on every row:

```
.../lexertl1.cpp.o.split/include/boost/spirit/home/support/detail/lexer/generator.hpp:10:10:
    fatal error: 'char_traits.hpp' file not found
```

`generator.hpp` opens with quoted, directory-relative includes:

```cpp
#include "char_traits.hpp"
#include "partition/charset.hpp"
#include "parser/tree/node.hpp"
```

A quoted include is resolved relative to the directory of the file doing the including. The
splitter writes a rewritten copy of `generator.hpp` into its mirror
(`<split_dir>/include/boost/spirit/home/support/detail/lexer/`), so that is now the directory
in question -- and the mirror holds only the headers that were themselves split:

```
$ ls <mirror>/boost/spirit/home/support/detail/lexer/
char_traits.hpp.split     <- a manifest saying it was skipped
generator.hpp             <- the rewritten copy
internals.hpp
...
```

`char_traits.hpp` is not there. Neither is any `-I` that would find it: the original is at
`boost/spirit/home/support/detail/lexer/char_traits.hpp` under an include root, so the
unqualified name resolves from nowhere on the search path.

The angle-bracket includes in the same file are fine, which is why this shape is rare enough
to have gone unnoticed: it needs a header that is split, that uses quoted relative includes,
and whose siblings are not all split themselves.

## What it costs, which is more than one unit

A fallback returns before `write_split_cache()`. There is no cache, so the next build repeats
everything: parse the unit, split every header candidate, fail, then compile plain. Measured on
a *timestamp-only* touch of a shared header, where nothing needs rebuilding at all:

| | wall |
|---|---:|
| the five units that split | **0.5s** |
| the one that falls back | **9.5s** |
| the same one, plain | 2.8s |

So one unit in six accounts for 95% of that row. This is the mechanism behind the warning the
other benchmarks carry -- that a fallback makes a row a plain build with the splitter's
overhead in front of it -- stated as a number.

## Reproduction

```sh
cmake -GNinja -S example/spirit-tests -B /tmp/sp \
    -DCMAKE_TOOLCHAIN_FILE=environments/monolithic.cmake \
    -DBOOST_ROOT_DIR=$PWD/example/boost-to-split \
    -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
CPP_SPLITTER_VERBOSE=1 ninja -C /tmp/sp -j8 spirit_test_lex_lexertl1
```

## Implementation spec

1. **Rewrite quoted includes when writing the copy.** While generating a rewritten header, walk
   its `#include "..."` directives. For each, resolve the target the way the compiler would --
   relative to the *original* header's directory first. If the target is itself mirrored, leave
   the directive alone: the mirror is structurally parallel, so the relative path still points
   at the right file. If it is not mirrored, rewrite it to the original's absolute path.

2. **Resolve, do not guess.** The decision is per directive and depends on whether that
   specific header was split, which `g_split_headers` and the mirror both already know. Do not
   blanket-rewrite every quoted include to an absolute path: a header pair that is *both*
   mirrored must keep pointing into the mirror, or the copy silently reads the original and the
   split-out definitions are defined twice.

3. **Leave angle-bracket includes alone.** They never resolve relative to the including file,
   so they are already correct through the existing `-I` list.

4. **Do not fix this by copying the siblings.** Copying an unsplit header into the mirror puts
   a second copy of it ahead of the real one on the include path for every consumer, which is
   the shadowing problem `TODO/06` and the "not parseable standalone" skip already exist to
   avoid.

## Acceptance Criteria

- `spirit_test_lex_lexertl1` splits with no fallback, and the program still passes.
- `./benchmark-spirit-tests.sh` reports **0 fallbacks** on every row, and its `one header` row
  becomes faster than plain rather than 2.55x slower.
- A regression fixture: a header that is split and that includes a sibling by quoted relative
  path, where the sibling is skipped (no include guard, or not parseable standalone). It must
  split, link and run.
- A second fixture for the case step 2 warns about: two headers in the same directory, both
  split, one including the other by quoted relative path. The definition the included one
  contributes must appear exactly once in the final object -- checked with `nm`, because it
  links either way.
