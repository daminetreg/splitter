// Covers an explicit function-template specialization whose `inline` arrives by macro.
//
// Boost.QVM writes
//
//     template <> BOOST_QVM_INLINE_TRIVIAL long double floor<long double>( long double x )
//
// where the macro carries an always-inline attribute *and* the `inline`. Stripping the
// attribute -- which has to happen, because an always-inline function is never emitted out of
// line -- took the `inline` with it. The text was then judged to open a template and left
// alone, so what remained was a strong definition in a header that every split piece
// includes, and `ld -r` rejected the copies:
//
//     multiple definition of `long double boost::qvm::floor<long double>(long double)'
//
// `template <>` introduces a function, not a template. It needs `inline` like any other, and
// the keyword has to go after the prefix: `inline template <>` is not a declaration.
//
// `scale` is the other half: a real template, which needs no `inline` and may not take one in
// front of its parameter list.
#pragma once

#define TRIVIAL inline __attribute__((__always_inline__))

template <class T> T rounded( T );
template <class T> T scale( T v ) { return v * 2; }

template <> TRIVIAL double rounded<double>( double x ) { return x < 0 ? -1.0 : 1.0; }
template <> TRIVIAL float  rounded<float>( float x )   { return x < 0 ? -1.0f : 1.0f; }
