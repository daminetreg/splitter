// Function: FilterResult filter_by_value_range(const std::vector<Record> &, double, double)
// Source: example/data_processing.cpp (lines 95-107)
// ---

#include "data_processing_preamble.h"

#line 95 "/home/runner/workspace/example/data_processing.cpp"
FilterResult filter_by_value_range(const std::vector<Record>& records,
                                    double min_val, double max_val) {
    FilterResult result;
    result.total_processed = static_cast<int>(records.size());
    for (const auto& r : records) {
        if (r.value >= min_val && r.value <= max_val) {
            result.matching.push_back(r);
        } else {
            result.rejected.push_back(r);
        }
    }
    return result;
}
