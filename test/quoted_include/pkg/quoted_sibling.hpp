#pragma once

// No function definitions and no namespace-scope variables, so the splitter writes no
// rewritten copy of this header into the mirror. That is the whole point: its sibling below
// includes it by relative path, and the mirror will not have it.
struct QuotedSiblingTag {
    enum { value = 40 };
};
