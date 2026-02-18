// Function: std::vector<Record> merge_records(const std::vector<Record> &, const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 199-211)
// ---

#include "data_processing_preamble.h"

#line 199 "/home/runner/workspace/example/data_processing.cpp"
std::vector<Record> merge_records(const std::vector<Record>& a,
                                   const std::vector<Record>& b) {
    std::vector<Record> merged;
    merged.reserve(a.size() + b.size());
    merged.insert(merged.end(), a.begin(), a.end());
    merged.insert(merged.end(), b.begin(), b.end());

    int next_id = 1;
    for (auto& r : merged) {
        r.id = next_id++;
    }
    return merged;
}
