#pragma once

// Namespace-scope definitions and not one function.
//
// `char const*` is the shape that catches people out: the *pointee* is const, the pointer is
// not, so each of these is a definition of an object with external linkage. Before TODO/32
// this header was skipped with "no function definitions", never rewritten, and therefore
// included verbatim by the preamble -- which every piece includes, so every piece compiled
// these definitions again and `ld -r` rejected the copies.
char const* hv_greeting = "hello";
int hv_count = 3;
