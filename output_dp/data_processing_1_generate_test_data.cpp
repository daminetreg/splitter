// Function: std::vector<Record> generate_test_data(int)
// Source: example/data_processing.cpp (lines 14-40)
// ---

#include "data_processing_preamble.h"

#line 14 "/home/runner/workspace/example/data_processing.cpp"
std::vector<Record> generate_test_data(int count) {
    std::vector<std::string> names = {
        "Alpha", "Beta", "Gamma", "Delta", "Epsilon",
        "Zeta", "Eta", "Theta", "Iota", "Kappa",
        "Lambda", "Mu", "Nu", "Xi", "Omicron"
    };
    std::vector<std::string> categories = {
        "electronics", "clothing", "food", "books", "sports"
    };

    std::vector<Record> records;
    records.reserve(count);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> name_dist(0, names.size() - 1);
    std::uniform_int_distribution<int> cat_dist(0, categories.size() - 1);
    std::uniform_real_distribution<double> val_dist(1.0, 1000.0);

    for (int i = 0; i < count; ++i) {
        Record r;
        r.id = i + 1;
        r.name = names[name_dist(rng)] + "_" + std::to_string(i);
        r.value = std::round(val_dist(rng) * 100.0) / 100.0;
        r.category = categories[cat_dist(rng)];
        records.push_back(std::move(r));
    }
    return records;
}
