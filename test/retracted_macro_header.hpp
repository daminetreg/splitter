// Covers the conditional context a split piece has to replay, and the two ways a header can
// make that context untrue by the time the piece is compiled.
//
// A definition written inside `#if C` is written into its split file inside the same
// `#if C`, so that a body the real build never sees is not compiled. But the piece replays
// the condition *after* including the whole header, which is a different point in the
// preprocessor's life:
//
//   * This file defines HAVE_FAST_PATH, writes `fast_double` under it, and undefines it
//     again on the last line -- the pattern Boost.Core's demangle.hpp uses. Replayed after
//     inclusion, `#if defined(HAVE_FAST_PATH)` is false.
//
//   * The include guard is spelled `#if !defined(...)` rather than `#ifndef`, which is the
//     more common of the two in Boost. Replayed after inclusion it is false as well.
//
// Either way the definition is preprocessed away and the piece compiles to an empty object.
// Nothing fails at that point: the failure surfaces only at the final link, as an undefined
// reference to a function whose definition is plainly there in the source.
#if !defined(CPP_SPLITTER_TEST_RETRACTED_MACRO_HPP)
#define CPP_SPLITTER_TEST_RETRACTED_MACRO_HPP

#define HAVE_FAST_PATH

namespace demo {

#if defined(HAVE_FAST_PATH)

inline int fast_double(int n) { return n * 2; }

#else

inline int fast_double(int n) { return n + n; }

#endif

// Guarded by nothing but the include guard, which is enough to break it on its own.
inline int triple(int n) { return n * 3; }

}  // namespace demo

#undef HAVE_FAST_PATH

#endif  // CPP_SPLITTER_TEST_RETRACTED_MACRO_HPP
