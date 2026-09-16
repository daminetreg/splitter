---
chapter: Benchmarks
chapter-label: The edit every benchmark makes
notes: The scenario the rest of the benchmarks are about. One line added to the body of standard_wide::toucs4, a static member function defined in its class in a header that 268 of the 279 units include. It is not a template, so the splitter can give it a piece; it is named inside templates in char_class.hpp and char.hpp, so most units carry it. A plain build recompiles all 268 units. What the split build does with the same edit is the body row of every table that follows: after TODO/49, one piece per unit, 267 compiles; after TODO/51, one shared piece, and 51 compiles for the header and the three headers whose include closure reaches it. Full, no-op and the two touch rows are there for the cost side; the body edit is the claim.
---
## The edit: {accent}one line in one body{/accent},  
in a header 268 units include.

```cpp
// boost/spirit/home/support/char_encoding/standard_wide.hpp
namespace boost { namespace spirit { namespace char_encoding {
    struct standard_wide
    {
        // ... isalnum, isalpha, isdigit, tolower, toupper ...

        static ::boost::uint32_t
        toucs4(wchar_t ch)
        {
            (void)4;  // benchmark probe   <- the edit
            return static_cast<make_unsigned<wchar_t>::type>(ch);
        }
    };
}}}
```

::: cards 3
- plain | | 268 translation units recompile: every one that includes the header.
- split, one piece per includer | violet | 267 pieces recompile, one per unit that keeps it; the copies and their PCHs are untouched.
- split, one shared piece | split | 51 compiles: `toucs4` once, with the other functions of its header and of the three headers that include it.
:::

::: tiny
`toucs4` is named by templates in `char_class.hpp` and `qi/char/char.hpp`, so no unit can declare it only; it is emitted by one unit.
:::
