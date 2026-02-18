// Function: std::vector<std::pair<Record, Record>> find_duplicates(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 225-239)
// ---

#include "data_processing_preamble.h"

#line 225 "/home/runner/workspace/example/data_processing.cpp"
std::vector<std::pair<Record, Record>> find_duplicates(const std::vector<Record>& records) {
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < records.size(); ++i) {
        std::string key = records[i].name + "|" + records[i].category;
        groups[key].push_back(i);
    }

    std::vector<std::pair<Record, Record>> duplicates;
    for (const auto& [key, indices] : groups) {
        for (size_t i = 1; i < indices.size(); ++i) {
            duplicates.push_back({records[indices[0]], records[indices[i]]});
        }
    }
    return duplicates;
}
