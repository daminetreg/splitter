# 26 — A `static` variable in the source gets a copy in every piece

**Severity:** High. A silently wrong program: the build succeeds, the link succeeds, and the
variable that was one object is now several.

## Motivation

`TODO/17` moved variables that may exist in only one object out of the preamble, and left
internal-linkage ones where they were on the grounds that a copy per piece is harmless because
internal linkage means a copy per translation unit anyway.

That is true of the *language* rule and false of what the splitter does with it. A translation
unit is split into many pieces, each of which includes the preamble and becomes an object of
its own. A `static` variable left in the preamble is therefore one object per **piece**, not
one per translation unit — and all of those objects are then merged into the single object the
build system asked for. The program ends up with several copies of a variable the source
declared once.

`example/static-init-order/` shows it in three files and no Boost:

```cpp
static Registrar s_registrar;          // constructor registers into a registry
```

```
registry().names[0] = "color_output"
registry().names[1] = "color_output"
...
registry().names[7] = "color_output"
```

Eight pieces, eight registrars, eight registrations. The source declares one.

Anything that counts, caches, registers or holds a flag behaves differently: a
`static int calls = 0;` shared by two functions counts in two objects; a `static bool done;`
guard runs its guarded work once per piece.

## What this is not

It is not what `TODO/23` declined. That was a rename map shared *between* translation units,
which matters only for a definition in a **header**: two units split the same header separately
and would have to agree on the mangled name or the shared split output is not shareable.

A `static` variable in the translation unit's own source has no such problem.
`build_static_rename_map()` is already built per unit from that unit's own definitions, and
that is exactly the scope this needs. `TODO/17` conflated the two and deferred the `.cpp` case
behind a decline that never applied to it; that paragraph is corrected there.

Variables in headers stay out of scope here, for the reason TODO 23 gives.

## Description

The treatment already exists — it is what `static` *functions* in a `.cpp` get:

> Split-out functions with internal linkage are renamed so that separately compiled objects
> can be combined with `ld -r` without symbol collisions.

A `static` function is moved into a piece, its `static` stripped, and its name mangled to
`__static_<stem>__<name>` so that two translation units that both define `helper()` do not
collide. Every use of the name — in the preamble, in the declarations, in the split bodies —
is rewritten through `apply_static_renames()`.

A `static` variable needs the same three things and currently gets none of them:

1. the definition moved to the definitions header, which exactly one piece compiles;
2. `static` stripped, because the other pieces have to be able to refer to it;
3. the name mangled, because it now has external linkage and may collide with another unit;

plus an `extern` declaration left where the definition was, so that the pieces and the
retained preamble text still see it.

## Implementation spec

1. **Harvest internal-linkage variables.** `harvest_variable()` records every namespace-scope
   variable but `prepare_variables()` drops the ones it will not move. Record the linkage on
   `VariableInfo` and keep them.

2. **Move them only from the unit's own source.** `input_is_header` is false. A header's
   internal-linkage definitions keep the treatment TODO 23 chose for functions.

3. **Skip an unnamed namespace.** A variable in one has internal linkage too, but hoisting it
   out changes which scope the name is looked up in, and any declaration the source already
   wrote inside that namespace stays there. `prepare_functions()` keeps unnamed-namespace
   functions for the same reason; do the same here rather than inventing a second rule.

4. **Mangle through the existing map.** Extend `build_static_rename_map()` to take the
   variables as well as the functions, so `apply_static_renames()` rewrites every use for free
   — including the uses inside other definitions, which is the part a hand-rolled rename would
   get wrong.

5. **Emit `extern` with the mangled name.** The declaration left behind is
   `extern <type> __static_<stem>__<name>;`. Build it from the mangled name directly:
   `replacement` is written into the preamble verbatim and is not passed through the rename
   map.

6. **Keep the existing bail-outs.** A declaration that defines a type, more than one
   declarator, a type that cannot be written in front of a name — all keep the treatment they
   have. Less moving is the safe direction.

Note this is orthogonal to the C++17 `inline` placement on this branch: `inline` gives a
variable *vague* linkage so the copies merge, which is exactly what a variable with *internal*
linkage must not have. The two rules do not overlap, and internal linkage is checked first.

## Acceptance Criteria

- `example/static-init-order/` registers **once**, split, at C++14 and at C++17.
- A regression fixture: a `static` counter at namespace scope in a `.cpp`, incremented by two
  functions that end up in different pieces, whose total the program checks.
- Two translation units that each define a `static` variable of the same name still link:
  the mangle has to make them distinct.
- Boost.Geometry's test suite, and the four-library harness, split with no new fallbacks.
