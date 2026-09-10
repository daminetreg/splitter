#pragma once

// A non-template member of a non-template class: the shape the splitter emits a piece for,
// and the one TODO/28's fast path exists to re-slice. side_info::collinear() in
// Boost.Geometry is the same shape.
struct Counter {
    inline int value() const
    {
        return 3 + 4;
    }

    // A second split-out definition, after the first. Its piece carries a #line directive
    // whose number moves when an edit above it adds a line, which is the part of the fast
    // path that is easiest to get wrong.
    inline int twice() const
    {
        return 2 * 21;
    }
};

// Included by the unit and called by nothing in it. Nothing needs an out-of-line copy, so no
// piece is emitted for it and the definition stays in this unit's rewritten copy of the
// header.
//
// That is the common case on a real corpus, not a curiosity: editing
// standard_wide::toucs4() in Boost.Spirit's suite reaches 194 units and exactly one of them
// emits it. The other 193 are this. An edit to such a body has to be re-sliced too -- by
// patching the kept copy rather than a piece -- or the units that merely include the header
// each pay a full parse, which on that corpus was 267 re-splits against 1.
inline int unused_by_this_unit()
{
    return 11;
}
