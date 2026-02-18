// Function: std::string format_summary(const Summary &)
// Source: example/data_processing.cpp (lines 165-175)
// ---

#include "data_processing_preamble.h"

#line 165 "/home/runner/workspace/example/data_processing.cpp"
std::string format_summary(const Summary& s) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "  Category: " << s.category << "\n"
        << "  Count:    " << s.count << "\n"
        << "  Total:    " << s.total << "\n"
        << "  Average:  " << s.average << "\n"
        << "  Min:      " << s.min_val << "\n"
        << "  Max:      " << s.max_val << "\n";
    return oss.str();
}
