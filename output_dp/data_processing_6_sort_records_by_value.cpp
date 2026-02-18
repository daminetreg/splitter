// Function: std::vector<Record> sort_records_by_value(std::vector<Record>, bool)
// Source: example/data_processing.cpp (lines 109-115)
// ---

#include "data_processing_preamble.h"

#line 109 "/home/runner/workspace/example/data_processing.cpp"
std::vector<Record> sort_records_by_value(std::vector<Record> records, bool ascending) {
    std::sort(records.begin(), records.end(),
        [ascending](const Record& a, const Record& b) {
            return ascending ? a.value < b.value : a.value > b.value;
        });
    return records;
}
