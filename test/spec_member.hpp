// Covers an explicit specialization of a member of a class template, defined in a source
// of its own, and the two things that go wrong when its definition is moved.
//
// The specialization is a strong symbol -- it may exist in exactly one object -- so it goes
// into the definitions header rather than the preamble. Two consequences:
//
//   * The class body does not declare it. What is written there is the primary template's
//     member, and C++ requires an explicit specialization to be declared before the first
//     use that would instantiate it instead. Moved out, the definition lands after every
//     such use, and the compiler rejects it: "explicit specialization after instantiation".
//     A declaration has to stay behind where the definition was.
//
//   * spec_member_impl.cpp defines nothing else, so every one of its definitions is kept in
//     the preamble and no split piece is compiled -- which means nothing includes the
//     definitions header, and the symbol is defined by no object at all.
//
// Boost.Serialization's xml_grammar.cpp is this file: one explicit specialization of
// basic_xml_grammar<char>::init_chset, an explicit instantiation below it, and nothing else.
#pragma once

// An ordinary function, so that splitting this header yields a piece that is really
// compiled: without one the translation unit has nothing to compile, falls back, and the
// missing definition never shows.
inline int bump(int n) { return n + 1; }

template <class T>
struct Box {
    T value;

    int weigh() const;

    // Instantiated below, and so a use of weigh() ahead of its specialization.
    int scaled() const { return weigh() * 3; }
};
