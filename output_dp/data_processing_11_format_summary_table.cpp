// Function: std::string format_summary_table(const std::map<std::string, Summary> &)
// Source: example/data_processing.cpp (lines 177-197)
// ---

#include "data_processing_preamble.h"

#line 177 "/home/runner/workspace/example/data_processing.cpp"
std::string format_summary_table(const std::map<std::string, Summary>& summaries) {
    std::ostringstream oss;
    oss << std::left << std::setw(15) << "Category"
        << std::right << std::setw(8) << "Count"
        << std::setw(12) << "Total"
        << std::setw(12) << "Average"
        << std::setw(12) << "Min"
        << std::setw(12) << "Max" << "\n";
    oss << std::string(71, '-') << "\n";

    oss << std::fixed << std::setprecision(2);
    for (const auto& [cat, s] : summaries) {
        oss << std::left << std::setw(15) << cat
            << std::right << std::setw(8) << s.count
            << std::setw(12) << s.total
            << std::setw(12) << s.average
            << std::setw(12) << s.min_val
            << std::setw(12) << s.max_val << "\n";
    }
    return oss.str();
}
