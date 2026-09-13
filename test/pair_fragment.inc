// A fragment of a function body, included by two functions of pair_header_main.cpp: no
// include guard, no definition, read twice, and not a header to split. OpenCV's
// ccl_bolelli_forest_singleline.inc.hpp is included this way by two functions of
// connectedcomponents.cpp; naming a second copy of it that was never written broke the unit.
// The macro is what tells the two inclusions apart, as OpenCV's ACTION_n do.
        v = PAIR_STEP(v);
