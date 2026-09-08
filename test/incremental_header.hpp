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
