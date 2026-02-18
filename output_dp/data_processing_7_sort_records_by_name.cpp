// Function: std::vector<Record> sort_records_by_name(std::vector<Record>)
// Source: example/data_processing.cpp (lines 117-123)
// ---

#include "data_processing_preamble.h"

#line 117 "/home/runner/workspace/example/data_processing.cpp"
std::vector<Record> sort_records_by_name(std::vector<Record> records) {
    std::sort(records.begin(), records.end(),
        [](const Record& a, const Record& b) {
            return a.name < b.name;
        });
    return records;
}
