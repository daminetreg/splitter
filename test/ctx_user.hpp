// Part of the TODO/09 fixture: deliberately does NOT include ctx_base.hpp. Like many real
// headers it is written to be included only after the types it uses are complete, so its
// split pieces have to be compiled in the context of the translation unit that includes
// it, not on their own.
#pragma once

namespace demo {

inline Weight heavier(Weight w) { return Weight{w.grams + 10}; }

inline int total(Weight w) { return heavier(w).doubled(); }

}  // namespace demo
