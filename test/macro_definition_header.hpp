// Covers a macro that expands to a single out-of-line member definition.
//
// When a definition's tokens come from a macro body, libclang reports its extent inside the
// invocation -- and what lands there can be a fragment of it. Boost.Test writes
//
//     BOOST_TEST_SINGLETON_CONS_IMPL(collector_t)
//
// which expands to one member function, and the extent reported for it is the two characters
// `t)`: the tail of the argument and the closing paren.
//
// That fragment used to be treated as the definition's text. It has external linkage and is
// not inline, so it went to the definitions header -- taking `t)` with it and leaving the
// invocation open. The next `#include` in the file was then read as a macro argument:
//
//     error: embedding a #include directive within macro arguments is not supported
//
// It took 21 of Boost.Geometry's 24 test targets down, because every one of them includes
// Boost.Test in header-only mode.
//
// The invocation is a unit: it produced this definition and nothing else the file refers to,
// since the class already declares the member. So the extent is widened to the whole
// invocation, which then moves to the definitions header intact.
#pragma once

#define SINGLETON_CONS_IMPL( type )                     \
  type& type::instance() {                              \
    static type the_inst; return the_inst;              \
  }                                                     \
/**/

struct collector {
    static collector& instance();
    collector& mark( int d );
    int v;
};
