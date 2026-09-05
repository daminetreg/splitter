// Covers a header edit made behind the prefix PCH.
//
// The include prefix -- the directives at the top of the source -- is precompiled so the
// include graph is parsed once per distinct prefix rather than once per split (TODO 14). The
// PCH is named after a hash of that prefix text, which does not change when one of the
// headers it names is edited.
//
// Nothing about that is safe on its own. libclang handed a PCH that no longer matches the
// files it was built from fails the parse outright with CXError_ASTReadError, and the unit
// falls back to compiling whole. The build still succeeds, so the only symptom is that the
// splitter has quietly stopped splitting -- which is exactly what it exists to do.
#pragma once

inline int contribute() {
    return 3 + 4;
}
