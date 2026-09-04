# 03 — Static-function renames are not applied to the generated preamble

**Severity:** High. Breaks every translation unit that takes the address of, or calls,
a split-out internal-linkage function from non-function code.

**Status: implemented and verified.** See "Outcome" at the end of this file.

## Motivation

Functions with internal linkage are renamed to `__static_<stem>__<name>` so that the
separately compiled split objects can be combined with `ld -r` without symbol
collisions. The rename is applied to the split function bodies and to the generated
forward declarations — but **not** to the preamble text, which is the original source
with function bodies carved out. Any reference to the function that lives outside a
split-out body still names the original symbol, which no longer exists.

Confirmed in `operations_preamble.h` (from `libs/filesystem/src/operations.cpp`):

```cpp
typedef int copy_file_data_t(int infile, int outfile, uintmax_t size, std::size_t blksize);

//! Pointer to the actual implementation of the copy_file_data implementation
copy_file_data_t* copy_file_data = &copy_file_data_read_write;   // <-- undeclared
```

and in `unique_path_preamble.h`:

```cpp
fill_random_t* fill_random = &fill_random_dev_random;            // <-- undeclared
```

Both yield `error: use of undeclared identifier`. Taking the address of an
implementation function to install it behind a function pointer is an extremely common
C++ idiom, so this will keep recurring on real codebases.

## Description

`generate_preamble()` (`src/main.cpp:358`) builds the preamble by walking the original
source and copying everything *except* the byte ranges of non-template function bodies,
then appending forward declarations:

```cpp
unsigned pos = 0;
for (const auto& r : ranges) {
    if (r.start > pos) {
        preamble += source.substr(pos, r.start - pos);   // verbatim, un-renamed
    }
    ...
}
if (pos < source.size()) {
    preamble += source.substr(pos);                      // verbatim, un-renamed
}
```

The rename map is built later and separately, in each of the two split drivers
(`src/main.cpp:1327` and `:1584`):

```cpp
std::vector<std::pair<std::string, std::string>> static_renames;
for (...) if (fn.is_static) static_renames.emplace_back(fn.name, make_static_mangled_name(stem, fn.name));

auto apply_static_renames = [&](std::string text) -> std::string { ... };
```

and is only ever applied to `body`. `generate_preamble()` never sees it.

The rename application itself already checks identifier boundaries on both sides, so it
will not rewrite `read_write` when renaming `read`. It does not, however, skip string
literals, character literals or comments, so an occurrence of the name inside a string
is still rewritten. `blank_code_noise()` (added for TODO 02) is the tool for this.

### Implementation plan

1. Build the rename map **before** generating the preamble, and pass it in:

   ```cpp
   static std::string generate_preamble(const std::string& source,
                                        const std::vector<FunctionInfo>& functions,
                                        const std::string& stem,
                                        const std::string& source_path,
                                        const StaticRenameMap& renames);
   ```

   Hoist the map construction out of `do_split()` / `do_split_with_cache()` into a small
   `build_static_rename_map(functions, stem)` used by all callers.
2. Apply the renames to every verbatim segment copied into the preamble — both the
   inter-body gaps and the trailing tail — not to the preamble as a whole after the
   fact, so that ranges stay aligned while building.
3. Make `apply_static_renames` literal- and comment-aware by running its search over
   `blank_code_noise(text)` (added for TODO 02) while applying edits to the original at
   the same offsets. Its existing identifier-boundary checks are already correct and
   should be kept.
4. Consider whether template bodies retained in the preamble
   (`should_keep_in_header(fn)`) also need renaming applied — they do, since a retained
   template can call a split-out static function.
5. ~~De-duplicate the two rename lambdas.~~ Done as part of TODO 02: `do_split()` and
   `do_split_with_cache()` now share one `emit_split_files()`, so there is a single
   `apply_static_renames` to change.

### Alternative considered

Not renaming at all, and instead emitting each internal-linkage function into the split
`.cpp` that uses it, was rejected: it defeats the point of splitting and does not work
when several split units reference the same helper. The rename is the right design; it
just has to be applied uniformly everywhere the identifier can appear.

## Acceptance Criteria

- `libs/filesystem/src/operations.cpp` and `libs/filesystem/src/unique_path.cpp` compile
  from their split pieces without falling back to plain compilation.
- No `error: use of undeclared identifier '<original static name>'` anywhere in a
  Boost `filesystem` split build log.
- Grepping any generated `*_preamble.h` for an original (un-mangled) static function
  name returns nothing.
- Regression fixtures covering:
  - a static function whose address is taken by a namespace-scope function pointer;
  - a static function called from a retained template body;
  - a static function named as a prefix of another identifier (e.g. `read` alongside
    `read_write`) — only the exact identifier may be rewritten;
  - the function's name appearing inside a string literal and inside a comment — both
    must be left alone.

## Outcome

Implemented in `src/main.cpp`:

- `build_static_rename_map()` and a free `apply_static_renames()` replace the per-driver
  lambda. Matching runs over `blank_code_noise()` output, so a renamed name occurring in a
  string literal or a comment is left alone, and replacements are applied back-to-front so
  offsets stay valid.
- `generate_preamble()` takes the rename map and applies it to everything it copies
  verbatim: the gaps between removed definitions, the trailing remainder of the file, the
  in-class declarations left behind for members, and retained template bodies (a template
  kept in the header can call a split-out static function).
- **Declarations for renamed functions are now emitted in place**, where the definition
  was, instead of being appended to the end of the preamble. The appended form arrives far
  too late for the case this item is about: `fn_ptr = &impl;` at namespace scope, a few
  lines below the definition, referred to a name nothing had declared yet.
- Renamed functions inside an unnamed namespace need their declaration in the scope the
  definition is hoisted to, so the unnamed namespace is closed before the declaration and
  reopened after it. Without this the declaration and the definition get different linkage
  and the reference goes unresolved. `FunctionInfo::unnamed_ns_depth` records the nesting.
- Declarations for functions whose name did *not* change stay appended at the end. Their
  uses all live inside other function bodies, which move into split files that include the
  whole preamble, so position does not matter -- and emitting them in place would mean
  rewriting macro-expanded and preprocessed headers, where a function's source extent is
  the macro invocation rather than a declarator. That attempt produced 2500 ambiguous
  operator errors and 2250 redefinitions before being narrowed.

Two bugs found and fixed along the way:

- **Duplicate source extents (pre-existing, from TODO 05).** Several functions can share
  one extent -- `BOOST_BITMASK(copy_options)` expands to nine operators, each reporting the
  macro invocation as its extent. The retained text was emitted once per function, so the
  macro was repeated nine times and redefined everything it declares. `generate_preamble()`
  now collapses identical extents.
- **Overloaded statics corrupted the rename.** Two overloads share one name, so the map
  held the entry twice and the rewriter replaced the same position twice, producing
  `__static_path_traits_cpp__convert_auxth_traits_cpp__convert_aux`. The map is now unique
  by name and hit positions are de-duplicated.

Verified on the Boost `filesystem` build:

| check | before | after |
|---|---|---|
| `use of undeclared identifier 'fill_random_dev_random'` | present | **0** |
| original static names left in any `*_preamble.h` | present | **0** |
| corrupted double-renamed identifiers | n/a | **0** |
| translation units failing on their own split pieces | 6 | **4** |

`unique_path.cpp`, `lock_pool.cpp` and `path_traits.cpp` no longer fail on their own split
pieces. `test/static_renames.cpp` covers a function pointer initialised at namespace scope
with the address of a split-out static, a call from a retained template body, a name that
is a prefix of another identifier, and the name appearing in a string literal and a
comment; it splits, compiles, links and runs.

The total error count is higher than before this change (2050 -> 2772) because translation
units that used to stop early now get further and reach unrelated failures. The remaining
blockers are all missing declaration context in split headers -- `no member named`,
`constexpr variable`, `non-constexpr declaration of` -- which belong with **TODO 06**,
plus **TODO 07** and **TODO 08**.
