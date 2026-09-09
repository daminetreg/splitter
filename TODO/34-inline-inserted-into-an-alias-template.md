# 34 — `inline` inserted into the middle of an alias template

**Severity:** High. The splitter rewrites a header into something that does not compile, and it
does it to Boost.Mp11, which most of Boost includes. The unit falls back, so nothing wrong
ships -- but a fallback writes no split cache, so it re-does the whole split and then compiles
plain on every build for ever (`TODO/31`).

## Motivation

Five of the 277 Boost.Spirit test units fall back in the cmake-re build:

```
spirit_test_qi_range_run
spirit_test_qi_regression_transform_assignment
spirit_test_qi_tst
spirit_test_support_utree
spirit_test_x3_container_support
```

all with the same three errors from `boost/mp11/algorithm.hpp`:

```
algorithm.hpp:1295:16: error: use of alias template 'detail::mp_pairwise_fold_impl' requires
                              template arguments
algorithm.hpp:1295:1:  error: type name does not allow function specifier to be specified
algorithm.hpp:1297:72: error: no template named 'mp_pairwise_fold_q'
```

## Description

Mp11 writes:

```cpp
template<class L, class Q> using mp_pairwise_fold_q =
    mp_eval_if<mp_empty<L>, mp_clear<L>, detail::mp_pairwise_fold_impl, L, Q>;
```

and the rewritten copy in the mirror reads:

```cpp
template<class L, class Q> using mp_pairwise_fold_q = mp_eval_if<mp_empty<L>, mp_clear<L>,
inline detail::mp_pairwise_fold_impl, L, Q>;
```

`inline` has been inserted **into the middle of a template argument list**. The declaration is
an *alias template*: `using X = ...;`, not a variable and not a function. There is nowhere in
it that a function specifier may appear, and the third error is only the first one propagating
-- once the alias fails to parse, every later use of `mp_pairwise_fold_q` is undeclared too.

The inline insertion is the C++17 in-place placement `TODO/32` and `TODO/33` rely on: rather
than move a namespace-scope definition that every piece would otherwise duplicate, the splitter
leaves it where it is and marks it `inline`. That is right for a variable. It is applied here
to a declaration that is not one, and `inline_insertion_point()` then picks an offset inside
the initialiser rather than in front of a declarator.

Two things are wrong and they are worth separating, because fixing only the second leaves the
first:

1. **An alias template is being harvested as a variable at all.** `using X = ...;` declares a
   type, and the existing bail-out for "a declaration that defines a type" should already have
   covered it. Ask libclang: `CXCursor_TypeAliasTemplateDecl` and `CXCursor_TypeAliasDecl` are
   not `CXCursor_VarDecl`, and neither is a `typedef`.

2. **`inline_insertion_point()` returned an offset it should have refused.** It is supposed to
   find where a declarator begins. Given text it does not understand it should return `npos`
   and the caller should leave the declaration alone, rather than inserting a keyword at
   whatever offset fell out. A specifier landing inside a template argument list is the
   signature of a position computed from the wrong thing.

## Reproduction

```sh
./build-spirit-cmake-re.sh --distributed --split --clean
grep -c 'falling back' /tmp/...   # 5
```

or, for one unit, split any translation unit that includes `boost/mp11/algorithm.hpp` and read
the rewritten copy at the line the error names.

## Why the standalone benchmark does not show it

`./benchmark-spirit-tests.sh` builds the same 277 targets at C++17 and reports **0 fallbacks**.
So something differs between the two builds and that difference is part of the question, not a
detail: the cmake-re project also configures the Boost superproject
(`BOOST_INCLUDE_LIBRARIES=spirit`), so the include set and the macro state a unit sees are not
identical. Find out which of the two conditions actually triggers the harvest before fixing, or
the fixture will be written for the wrong one.

## Implementation spec

1. **Refuse the declaration by cursor kind, not by text.** Whatever harvests this must exclude
   `CXCursor_TypeAliasDecl`, `CXCursor_TypeAliasTemplateDecl` and `CXCursor_TypedefDecl`. This
   is the same lesson `TODO/26` and `TODO/33` each paid for once: ask the type or the cursor,
   never the spelling.

2. **Make `inline_insertion_point()` fail closed.** When it cannot identify a declarator it
   must return `npos`, and the callers must treat that as "leave it alone". It already returns
   `npos` for a real template's parameter list; the same refusal belongs here.

3. **A fixture for each.** A header defining an alias template whose right-hand side mentions a
   qualified name, and one defining a variable template, split and compiled. Both must come out
   of the mirror byte-identical to their originals.

## Acceptance Criteria

- The five units above split with no fallback and their programs pass.
- The rewritten `boost/mp11/algorithm.hpp` is byte-identical to the original: nothing in it is
  a candidate for anything.
- `./benchmark-spirit-tests.sh` still reports 0 fallbacks, and the four-library harness is
  unchanged.
