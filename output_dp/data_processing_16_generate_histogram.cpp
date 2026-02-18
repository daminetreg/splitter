// Function: std::string generate_histogram(const std::vector<Record> &, int)
// Source: example/data_processing.cpp (lines 262-296)
// ---

#include "data_processing_preamble.h"

#line 262 "/home/runner/workspace/example/data_processing.cpp"
std::string generate_histogram(const std::vector<Record>& records, int buckets) {
    if (records.empty() || buckets <= 0) return "(no data)\n";

    double min_val = records[0].value, max_val = records[0].value;
    for (const auto& r : records) {
        min_val = std::min(min_val, r.value);
        max_val = std::max(max_val, r.value);
    }

    double range = max_val - min_val;
    if (range < 0.001) range = 1.0;
    double bucket_size = range / buckets;

    std::vector<int> counts(buckets, 0);
    for (const auto& r : records) {
        int idx = static_cast<int>((r.value - min_val) / bucket_size);
        if (idx >= buckets) idx = buckets - 1;
        counts[idx]++;
    }

    int max_count = *std::max_element(counts.begin(), counts.end());
    int bar_width = 40;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    for (int i = 0; i < buckets; ++i) {
        double lo = min_val + i * bucket_size;
        double hi = lo + bucket_size;
        int bar_len = (max_count > 0) ? (counts[i] * bar_width / max_count) : 0;
        oss << std::setw(8) << lo << " - " << std::setw(8) << hi
            << " |" << std::string(bar_len, '#')
            << " (" << counts[i] << ")\n";
    }
    return oss.str();
}
