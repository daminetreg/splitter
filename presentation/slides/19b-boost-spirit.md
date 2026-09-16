---
chapter: Benchmarks
chapter-label: Boost.Spirit on EngFlow RBE
notes: Before the numbers, what is being built. Boost.Spirit is a parser and generator library written entirely as templates in headers -- a grammar is a C++ expression, char_('0', '9') is a parser object, operators compose them. Its test suite is 277 small programs, each including a large part of Boost and instantiating it. Almost nothing the suite compiles is the suite's own code. On the left, a typical test: BOOST_TEST over the parsers; on the right, the shape everyone knows Spirit for. Header-only, template-heavy: the hardest case for a per-function split, and the one the benchmarks use.
---
## Boost.Spirit: {violet}277 test programs{/violet},  
header-only, template-heavy.

::: code-columns
```cpp
// libs/spirit/test/qi/char1.cpp
#include <boost/spirit/include/qi_char.hpp>
#include <boost/spirit/include/qi_action.hpp>
#include "test.hpp"

int main()
{
    using spirit_test::test;
    using namespace boost::spirit::ascii;

    BOOST_TEST(test("x", char_));
    BOOST_TEST(test("x", char_('x')));
    BOOST_TEST(!test("x", char_('y')));
    BOOST_TEST(test("x", char_('a', 'z')));
    BOOST_TEST(!test("x", char_('0', '9')));
    BOOST_TEST(test(" ", ~char_('x')));
    return boost::report_errors();
}
```
---
```cpp
// what a Spirit grammar looks like
#include <boost/spirit/include/qi.hpp>
namespace qi = boost::spirit::qi;

bool parse_numbers(std::string const& s,
                   std::vector<double>& out)
{
    auto first = s.begin();
    bool ok = qi::phrase_parse(
        first, s.end(),
        qi::double_ % ',',   // the grammar
        qi::space,           // the skipper
        out);
    return ok && first == s.end();
}
```
:::

::: tiny
279 translation units, each pulling in some 1,500 headers. The grammars are template expressions instantiated in every unit that spells them; the splitter's pieces are what is left once the templates stay in the preamble.
:::
