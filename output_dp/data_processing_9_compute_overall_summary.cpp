// Function: Summary compute_overall_summary(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 145-163)
// ---

#include "data_processing_preamble.h"

#line 145 "/home/runner/workspace/example/data_processing.cpp"
Summary compute_overall_summary(const std::vector<Record>& records) {
    Summary s;
    s.category = "ALL";
    s.count = static_cast<int>(records.size());
    if (records.empty()) {
        s.total = s.average = s.min_val = s.max_val = 0.0;
        return s;
    }
    s.total = 0.0;
    s.min_val = records[0].value;
    s.max_val = records[0].value;
    for (const auto& r : records) {
        s.total += r.value;
        s.min_val = std::min(s.min_val, r.value);
        s.max_val = std::max(s.max_val, r.value);
    }
    s.average = s.total / s.count;
    return s;
}
