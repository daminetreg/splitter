// Function: FilterResult filter_by_category(const std::vector<Record> &, const std::string &)
// Source: example/data_processing.cpp (lines 81-93)
// ---

#include "data_processing_preamble.h"

#line 81 "/home/runner/workspace/example/data_processing.cpp"
FilterResult filter_by_category(const std::vector<Record>& records,
                                 const std::string& category) {
    FilterResult result;
    result.total_processed = static_cast<int>(records.size());
    for (const auto& r : records) {
        if (r.category == category) {
            result.matching.push_back(r);
        } else {
            result.rejected.push_back(r);
        }
    }
    return result;
}
