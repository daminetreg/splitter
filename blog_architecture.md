# Splitting C++ One Function at a Time: an Architecture, and the Library That Wrote It

## The naive idea

C++ builds are slow because the translation unit is the unit of work. Change one
function in a 3000-line `.cpp` and the compiler re-parses every header it includes,
re-instantiates every template, and re-emits everything.

The naive idea: what if the unit of work were the *function*?

Parse the source with libclang, find every function definition, write each one into its
own tiny `.cpp`, compile those in parallel, and staple the objects back together with
`ld -r` into the single `.o` the build system expects. Drop the tool in as
`CMAKE_CXX_COMPILER_LAUNCHER` and nothing upstream needs to know.

```
      foo.cpp
         |
    [ libclang ]
         |
    +----+----+----+
    |    |    |    |
  fn_1 fn_2 fn_3 fn_4          one .cpp per function
    |    |    |    |
   .o   .o   .o   .o           compiled in parallel
    +----+----+----+
         |
      [ ld -r ]
         |
       foo.o                   what the build system asked for
```

Each piece needs the declarations the original file had — the includes, the classes, the
typedefs. So the splitter also emits a **preamble**: the original source with the
function bodies carved out. Every piece includes it.

```
  foo.cpp                        foo_preamble.h
  ---------                      --------------
  #include <vector>              #include <vector>
  struct S { ... };      ==>     struct S { ... };
  int a() { X }                  int a();        <- body removed, declaration left
  int b() { Y }                  int b();

                                 foo_1_a.cpp:  #include "foo_preamble.h"
                                               int a() { X }
```

That is the whole architecture. It is also almost entirely wrong, and the rest of this
post is about the ways real C++ found to prove it.

---

## Step 1: the preamble is a cache, not a convenience

The first problem is obvious in hindsight. If every piece includes the preamble, and the
preamble includes `<vector>` and `<boost/filesystem/path.hpp>`, then splitting a file
into 50 functions parses those headers 50 times. The "parallel" build is slower than the
serial one it replaced.

The fix is a precompiled header. The preamble is generated once, compiled to a PCH, and
every piece picks it up.

```
  foo_preamble.h --[ compile ]--> foo_preamble.h.gch/<content-hash>.gch
                                            |
                       +--------------------+--------------------+
                       |                    |                    |
                   foo_1_a.cpp          foo_2_b.cpp          foo_3_c.cpp
```

Two decisions worth recording.

**PCHs are named by content hash, not timestamp.** `<preamble>.gch/<hash>.gch` exists if
and only if that exact preamble has been compiled. No staleness logic, no clock skew, and
the artifact is distributable — it can be shipped to a remote build node or a cache
without a coordinating server.

**There are two PCH systems, for two different consumers.** A GCC-style `.gch` for the
real compiler building the pieces, and a libclang `.pch` for the splitter's own parsing.
They serve different tools and have different lifetimes.

There is a trap here that took a while to find. The preamble is *this very file* with the
bodies carved out, so it declares every class and typedef the file declares. Feeding its
PCH back in while parsing that same file redefines all of them. The first run got away
with it because no preamble existed yet; from the second run on the parse was wrecked,
the visitor found almost nothing, and the stale-output pruning deleted the previous run's
work. A twelve-function file became a one-function file. The preamble's PCH is for
compiling the pieces, never for re-parsing their origin.

---

## Step 2: from one preamble to many

A `.cpp` in a modern C++ project barely contains any code. The code is in the headers —
inline functions, class members defined in-class, templates. Splitting only the `.cpp`
splits the tip of the iceberg.

So the splitter follows `clang_getInclusions()` and splits the headers too. Each header
gets *its own* preamble — a rewritten copy of itself with the bodies removed — and its
own pieces. One translation unit now produces a small tree of preambles.

```
      foo.cpp
         |
    [ libclang ]  ..... one parse .....
         |
    +----+-------------------------------+
    |                                    |
  foo's own functions            each included header
    |                                    |
  foo_preamble.h                  include/boost/filesystem/path.hpp        <- rewritten
  foo_1_a.cpp                     include/boost/filesystem/path.hpp_1_append.cpp
  foo_2_b.cpp                     include/boost/filesystem/path.hpp_2_compare.cpp
    |                             include/boost/system/detail/error_code.hpp
    |                             include/boost/system/detail/error_code.hpp_1_what.cpp
    |                                    |
    +---------------[ ld -r ]------------+
                        |
                      foo.o
```

Headers were originally re-parsed standalone to find out what they defined. That requires
every header to be parseable on its own, which many are not, and costs one full libclang
parse per header per including source — 2769 of them across twelve translation units on
the example below.

**Decision: harvest from the parent translation unit.** The parent already contains every
function defined in every header it includes, parsed in the right macro and inclusion
context. One AST walk yields the inventory for the source *and* for every header it should
split. Header re-parses went to zero and wall time dropped from 46.6s to 26.5s.

This is where the design gets interesting, because a header is not a `.cpp`. It is
included by many translation units, it may not be self-contained, and the things in it
have linkage rules a `.cpp` never exercises.

---

## The benchmark

Everything below was found by pointing the splitter at **Boost.Filesystem** — twelve
translation units pulling in Boost.System, Boost.Atomic, Boost.SmartPtr, Boost.Iterator,
Boost.Scope and a large slice of Boost.Config.

The progression, measured on that example:

| stage | TUs linking from split objects | fallbacks | program linked against the result |
|---|---|---|---|
| initial | 2 of 12 | 9 | fails |
| member out-lining | 3 of 12 | 8 | fails |
| includer context for header pieces | 10 of 12 | 1 | 19 undefined refs |
| emit-reachability | 10 of 12 | 1 | **links, 9/9 assertions** |
| layered preamble | **12 of 12** | **0** | **links, 9/9 assertions** |

Each row is an architectural decision forced by a specific linkage rule. Here they are.

---

## Case 1: a member function is not a free function

A member defined inside its class body:

```cpp
class Widget {
public:
    int value() const { return v_; }
};
```

Carved out verbatim into a piece, this is not valid code. `const` is illegal on a
non-member, `this` is unavailable, and a constructor's member-initialiser list parses as a
base-initialiser on a free function.

The piece has to be *rewritten*, not moved:

```
   in-class                          out-of-line piece
   --------                          -----------------
   int value() const        ==>      auto Widget::value() const -> int
   { return v_; }                    { return v_; }

   preamble keeps:                   int value() const;   <- declaration, inside the class
```

**Return types move to a trailing return type.** A leading return type is looked up in the
*enclosing namespace* once out-of-line, so a class-scoped name like `result_type` or
`iterator` stops resolving. A trailing return type is looked up in class scope. One
rewrite fixes every such name at once.

**The type text comes from libclang, not the source span.** The span before the function
name also holds function specifiers, which are routinely macros — `BOOST_FORCEINLINE` —
and are textually indistinguishable from a type name.

**The test for "defined in-class" is the *lexical* parent, not the semantic one.** A
definition already written out-of-line (`void path::foo() {}`) also has a class as its
semantic parent. Getting this wrong emitted a stray declaration for every one of them:
2900 errors.

Some members cannot be moved at all and stay in the header: members of class templates,
explicit specializations, virtuals (whose `override` is usually a macro), and constructors
and destructors — which are not one symbol but a *family*, C1/C2/C3 and D0/D1/D2, where
the compiler decides which members of it to emit. Splitting one emitted the base-object
constructor while every caller asked for the complete-object one, and the link failed on a
symbol that `nm -C` cheerfully reported as present, because both variants demangle to the
same text.

---

## Case 2: a split header is not self-contained

`boost/system/detail/std_category_impl.hpp` is written to be included only after
`error_condition` is complete. Compile a piece of it that includes only that header and
you get `calling 'default_error_condition' with incomplete return type`.

**Decision: a piece taken from a header includes the translation unit's preamble first.**
That replays the include prefix the header was actually seen behind.

```
   include/boost/system/detail/std_category_impl.hpp_1_equivalent.cpp
   -----------------------------------------------------------------
   #include "operations_preamble.h"      <- the includer's context, replayed
   #include "std_category_impl.hpp"      <- the header itself
   inline bool std_category::equivalent(...) { ... }
```

This exposed a second bug immediately. The split tree was appended *after* the project's
own `-I` flags, so an original header won the lookup over its rewritten copy — bringing
back the definitions that had been moved into pieces. 1008 `redefinition of` errors, every
translation unit falling back. The split directory and its mirrored include root now come
first.

And a third: a quote include resolves relative to the including file, but the preamble no
longer sits in the source's directory, so a sibling header could not be found. The unit's
source directory is now an implicit include directory.

---

## Case 3: a flat output directory shadows the wrong header

Split headers were originally written flat, one file per basename. Boost has many repeated
basenames.

```
   libs/filesystem/src/atomic_ref.hpp              --+
                                                     +--> atomic_ref.hpp   <- one wins
   libs/atomic/include/boost/atomic/atomic_ref.hpp --+
```

The survivor then shadowed the real header for *every* consumer, because the split
directory is on the include path. Here it produced a compile error; with two
compatible-looking headers it would have produced a silently miscompiled object.

**Decision: mirror each header at the path it was included as.**

```
   <split_dir>/
     foo_preamble.h
     foo_1_a.cpp
     include/
       boost/filesystem/path.hpp          <- rewritten, at its real relative path
       boost/atomic/atomic_ref.hpp
       atomic_ref.hpp                     <- the other one, no longer colliding
```

Resolved by longest-matching `-I` directory, with a collision guard that fails loudly if
two sources ever map to one output path.

The same flat layout had a second facet. A source and a header sharing a stem (`path.cpp`
and `path.hpp`) shared one piece-naming scheme, and the stale-output pruning deleted all 54
of `path.cpp`'s pieces as "stale" because they were not in `path.hpp`'s list of 154. Pieces
are now named by full file name, not stem.

---

## Case 4: preprocessor context is part of the definition

A definition inside `#if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)`, emitted
unconditionally, is a definition referring to a type that may not exist.

**Decision: capture the conditional stack active at the definition and replay it around
the piece.**

```
   piece for path(std::basic_string_view<value_type> const&)
   ---------------------------------------------------------
   #include "portability_preamble.h"
   #include "path.hpp"
   #if !defined(BOOST_NO_CXX17_HDR_STRING_VIEW)      <- replayed
   namespace boost { namespace filesystem {
   inline path::path(std::basic_string_view<value_type> const& s) : m_pathname(s) {}
   } }
   #endif
```

With the file's own include guard excluded — its macro is already defined by the time the
piece compiles, so replaying it would delete the body — and `#elif C` rewritten to
`#if C`, since a piece opens its own conditional rather than continuing someone else's.

A related one: a macro `#define`d before a body and `#undef`d later in the same header —
Boost.Assert does exactly this with `BOOST_ASSERT_SNPRINTF` — cannot be moved at all,
because the piece includes the whole header first and the `#undef` has already run.

---

## Case 5: split only what the compiler would actually emit

This is the subtlest, and the one that finally made the library link.

Boost.Filesystem hides a block of inline forwarders behind a guard:

```cpp
// To avoid ODR violation, these functions are not defined when the library itself is built.
#if !defined(BOOST_FILESYSTEM_SOURCE)
    BOOST_FORCEINLINE path& path::append(...)
    BOOST_FORCEINLINE path  path::generic_path() const
    ...
#endif
```

The library is built *with* `-DBOOST_FILESYSTEM_SOURCE`. Inside it, those functions are
declared and never defined — deliberately, so that only user code carries them. That is
harmless normally: the functions that *call* them are inline too, nobody odr-uses them,
they are never emitted, and the calls never materialise. The unsplit library defines none
of them either.

Splitting breaks the balance. Every split piece is forced into existence with
`__attribute__((used))`, so a body the build never wanted becomes real code, and its calls
to those forwarders become references nothing resolves. Nineteen undefined symbols at
final link.

There is no property of the declaration to test — from inside the library those functions
look exactly like ordinary ones defined in another object. Counting every reference in the
translation unit is not enough either: a call inside a body that is itself never emitted
does not make its target needed.

**Decision: split only what the translation unit actually emits.**

```
   roots  =  non-inline definitions with external linkage
             + namespace-scope initialisers (they run regardless)
                              |
                    [ BFS over the call graph ]
                              |
             reachable set  =  what the compiler will emit
                              |
        split a definition only if it lands in that set;
        everything else stays in the header, costing nothing
```

The two error directions are not symmetric, and that asymmetry is the whole design
rationale. *Missing* a root leaves a function in the header that could have been split —
less splitting, nothing broken. *Over-including* produces dangling references. So the roots
are kept deliberately narrow.

Undefined references went from 19 to zero, and the build got faster (23.7s to 16.5s),
because less is forced into existence.

---

## Case 6: the preamble is included many times

The last one. Every piece includes the preamble, so everything the preamble carries is
compiled once *per piece*. Fine for declarations. Fine for vague linkage — that is exactly
what `inline` is for. Not fine for an ordinary definition with external linkage.

`filesystem_error`'s virtual destructor is kept in the preamble on purpose: moving a
virtual out-of-line means stripping an `override` that is usually a macro. It is also
non-inline with external linkage. Every piece emitted it:

```
exception_preamble.h:54: multiple definition of `filesystem_error::~filesystem_error()'
exception_preamble.h:54: multiple definition of `typeinfo for filesystem_error'
```

**Decision: layer the preamble by linkage.**

```
   +--------------------------------------------+
   |  foo_preamble.h              (tier one)    |   declarations
   |    classes, enums, typedefs                |   + vague-linkage definitions
   |    inline / template / constexpr / static  |   INCLUDED BY EVERY PIECE
   +--------------------------------------------+
             ^            ^             ^
             |            |             |
       foo_1_a.cpp   foo_2_b.cpp   foo_3_c.cpp
             |
             | #include            (exactly one piece does this)
             v
   +--------------------------------------------+
   |  foo_definitions.h           (tier two)    |   must appear exactly once
   |    non-inline, external linkage            |   INCLUDED BY ONE PIECE
   +--------------------------------------------+
```

Tier one keeps a declaration where the definition was — nothing for a member, which its
class already declares; a forward declaration for a free function. Tier-two text is
re-wrapped in the namespaces it was lifted out of.

This is better than out-lining those definitions, for three reasons:

- **It leaves the source text alone**, so the macro-spelled `override` problem never
  arises and the keep-virtuals rule stops conflicting with this one.
- **The key-function consequence falls out for free.** The vtable and typeinfo follow the
  first non-inline virtual, so they land in whichever piece includes tier two — exactly one
  object, by construction rather than by a rule someone has to remember.
- **It generalises to namespace-scope variables**, which out-lining cannot reach at all: a
  variable cannot become a function piece.

"Safe to include many times" becomes a property of *how the preamble is built*, rather than
something every keep rule has to get right independently.

With that, Boost.Filesystem splits **12 of 12 translation units, zero fallbacks**, no
`multiple definition` anywhere, `filesystem_error`'s typeinfo appearing exactly as often as
in an unsplit build, and a program linked against the result passing the same nine
filesystem assertions as one linked against a normal build.

---

## The architecture, assembled

```
   foo.cpp
      |
      |  [ 1 ]  ONE libclang parse of the translation unit
      v
   +---------------------------------------------------------------+
   |  harvest: every definition, bucketed by defining file          |
   |  emit-graph: roots -> reachable set (what will be emitted)     |
   +---------------------------------------------------------------+
      |
      |  [ 2 ]  per unit (the .cpp, and each header it should split)
      v
   +---------------------------------------------------------------+
   |  classify each definition                                      |
   |    keep in header?   template / virtual / ctor / unnamed-ns /  |
   |                      constexpr / always-inline / not emitted   |
   |    vague linkage?    inline / template / internal linkage      |
   +---------------------------------------------------------------+
      |
      +--> tier one preamble  (declarations + vague-linkage defs)  --> .pch
      +--> tier two defs      (external-linkage defs, once only)
      +--> one piece per split definition
             #include tier one
             #include includer's preamble   (header pieces only)
             #include tier two              (exactly one piece)
             replayed #if context
             rewritten out-of-line definition
      |
      |  [ 3 ]  compile pieces in parallel, ld -r
      v
   foo.o
```

Every box in that diagram exists because a specific C++ linkage rule made the simpler
version wrong.

---

## Why Boost.Filesystem is the right benchmark

Almost every architectural decision above was forced by Boost.Filesystem, and almost none
of them by the small hand-written fixtures added alongside. Several times a fixture was
written to reproduce a Boost failure and simply refused to fail, because the specific
combination of linkage properties that produced it did not occur.

The reason is that Boost.Filesystem happens to exercise nearly every linkage and symbol
category C++ has, in one modestly-sized library:

- **Internal linkage** — anonymous-namespace helpers, and `static` free functions that
  must be renamed to survive being split across objects at all.
- **Vague linkage** — inline functions, function templates, class templates, explicit
  specializations: definitions that may legally appear in every object.
- **External linkage** — `BOOST_FILESYSTEM_DECL` definitions that must appear in exactly
  one.
- **`available_externally`** — `BOOST_FORCEINLINE` functions emitted in *no* object at
  all, meant to be inlined at every call site, and impossible to link against.
- **Symbol families** — constructors (C1/C2/C3) and destructors (D0/D1/D2), where a split
  definition can emit one variant while callers need another, and the demangled names are
  identical so the tooling lies to you.
- **Key functions** — virtual members that drag the vtable and typeinfo to wherever they
  are emitted.
- **Inline namespaces** — `BOOST_FILESYSTEM_VERSION_NAMESPACE` versioning, with `using`
  declarations naming functions whose definitions have moved.
- **Conditional definitions** — the `#if !defined(BOOST_FILESYSTEM_SOURCE)` block that
  exists in user code and deliberately does not exist in the library.
- **Header/footer pairs** — `detail/header.hpp` and `detail/footer.hpp`, files written
  without include guards, meant never to stand alone.
- **Implementation includes** — `utf8_codecvt_facet.ipp`, an entire translation unit's
  worth of code in a file that is not called a header and has no guard.
- **Macro-spelled everything** — `BOOST_FORCEINLINE`, `BOOST_OVERRIDE`,
  `BOOST_SYSTEM_CONSTEXPR`, `BOOST_UTF8_DECL`: specifiers that no amount of text matching
  will find, and that only the AST can answer.

A tool that splits C++ into one function per object file is, in effect, a simultaneous
stress test of every one of those rules — because splitting takes each definition out of
the context that made its linkage correct, and the tool's entire job is putting that
context back. Boost.Filesystem is small enough to iterate on in under twenty seconds and
rich enough that essentially every category shows up, usually in a form where the naive
implementation looks like it works.

It has been less a test case than a specification.

The naive idea — one function, one object file — turned out to be about two percent of the
work. The other ninety-eight percent is linkage.
