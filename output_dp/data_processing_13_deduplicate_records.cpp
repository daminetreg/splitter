// Function: std::vector<Record> deduplicate_records(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 213-223)
// ---

#include "data_processing_preamble.h"

#line 213 "/home/runner/workspace/example/data_processing.cpp"
std::vector<Record> deduplicate_records(const std::vector<Record>& records) {
    std::set<std::string> seen;
    std::vector<Record> unique;
    for (const auto& r : records) {
        std::string key = r.name + "|" + r.category;
        if (seen.insert(key).second) {
            unique.push_back(r);
        }
    }
    return unique;
}
