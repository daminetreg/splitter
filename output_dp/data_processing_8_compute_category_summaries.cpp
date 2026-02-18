// Function: std::map<std::string, Summary> compute_category_summaries(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 125-143)
// ---

#include "data_processing_preamble.h"

#line 125 "/home/runner/workspace/example/data_processing.cpp"
std::map<std::string, Summary> compute_category_summaries(const std::vector<Record>& records) {
    std::map<std::string, std::vector<double>> category_values;
    for (const auto& r : records) {
        category_values[r.category].push_back(r.value);
    }

    std::map<std::string, Summary> summaries;
    for (const auto& [cat, values] : category_values) {
        Summary s;
        s.category = cat;
        s.count = static_cast<int>(values.size());
        s.total = std::accumulate(values.begin(), values.end(), 0.0);
        s.average = s.total / s.count;
        s.min_val = *std::min_element(values.begin(), values.end());
        s.max_val = *std::max_element(values.begin(), values.end());
        summaries[cat] = s;
    }
    return summaries;
}
