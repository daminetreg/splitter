#include "data_processing.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <cassert>
#include <set>
#include <functional>
#include <regex>
#include <random>

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

std::vector<Record> parse_csv_records(const std::string& csv_data) {
    std::vector<Record> records;
    std::istringstream stream(csv_data);
    std::string line;

    std::getline(stream, line);

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        Record r;
        std::string token;

        std::getline(ls, token, ',');
        r.id = std::stoi(token);

        std::getline(ls, r.name, ',');

        std::getline(ls, token, ',');
        r.value = std::stod(token);

        std::getline(ls, r.category, ',');

        records.push_back(std::move(r));
    }
    return records;
}

std::string records_to_csv(const std::vector<Record>& records) {
    std::ostringstream oss;
    oss << "id,name,value,category\n";
    for (const auto& r : records) {
        oss << r.id << "," << r.name << ","
            << std::fixed << std::setprecision(2) << r.value << ","
            << r.category << "\n";
    }
    return oss.str();
}

FilterResult filter_by_category(const std::vector<Record>& records,
                                 const std::string& category) {
    FilterResult result;
    result.total_processed = static_cast<int>(records.size());
    for (const auto& r : records) {
        if (r.category == category) {
            result.matching.push_back(r);
        } else {
            result.rejected.push_back(r);
        }
    }
    return result;
}

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

std::vector<Record> sort_records_by_value(std::vector<Record> records, bool ascending) {
    std::sort(records.begin(), records.end(),
        [ascending](const Record& a, const Record& b) {
            return ascending ? a.value < b.value : a.value > b.value;
        });
    return records;
}

std::vector<Record> sort_records_by_name(std::vector<Record> records) {
    std::sort(records.begin(), records.end(),
        [](const Record& a, const Record& b) {
            return a.name < b.name;
        });
    return records;
}

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

std::string format_summary(const Summary& s) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "  Category: " << s.category << "\n"
        << "  Count:    " << s.count << "\n"
        << "  Total:    " << s.total << "\n"
        << "  Average:  " << s.average << "\n"
        << "  Min:      " << s.min_val << "\n"
        << "  Max:      " << s.max_val << "\n";
    return oss.str();
}

std::string format_summary_table(const std::map<std::string, Summary>& summaries) {
    std::ostringstream oss;
    oss << std::left << std::setw(15) << "Category"
        << std::right << std::setw(8) << "Count"
        << std::setw(12) << "Total"
        << std::setw(12) << "Average"
        << std::setw(12) << "Min"
        << std::setw(12) << "Max" << "\n";
    oss << std::string(71, '-') << "\n";

    oss << std::fixed << std::setprecision(2);
    for (const auto& [cat, s] : summaries) {
        oss << std::left << std::setw(15) << cat
            << std::right << std::setw(8) << s.count
            << std::setw(12) << s.total
            << std::setw(12) << s.average
            << std::setw(12) << s.min_val
            << std::setw(12) << s.max_val << "\n";
    }
    return oss.str();
}

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

std::string generate_report(const std::vector<Record>& records) {
    std::ostringstream oss;
    oss << "=== Data Processing Report ===\n\n";

    auto overall = compute_overall_summary(records);
    oss << "Overall Summary:\n" << format_summary(overall) << "\n";

    auto cat_summaries = compute_category_summaries(records);
    oss << "Category Breakdown:\n" << format_summary_table(cat_summaries) << "\n";

    auto top5 = sort_records_by_value(records, false);
    oss << "Top 5 Records by Value:\n";
    for (int i = 0; i < std::min(5, static_cast<int>(top5.size())); ++i) {
        oss << "  " << top5[i].id << ". " << top5[i].name
            << " (" << top5[i].category << "): "
            << std::fixed << std::setprecision(2) << top5[i].value << "\n";
    }

    return oss.str();
}

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

void run_all_tests() {
    std::cout << "=== Data Processing Tests ===\n\n";

    auto data = generate_test_data(50);
    assert(data.size() == 50);
    std::cout << "[PASS] generate_test_data: created " << data.size() << " records\n";

    auto csv = records_to_csv(data);
    auto parsed = parse_csv_records(csv);
    assert(parsed.size() == data.size());
    assert(parsed[0].name == data[0].name);
    std::cout << "[PASS] CSV round-trip: " << parsed.size() << " records preserved\n";

    auto cat_filter = filter_by_category(data, "electronics");
    assert(cat_filter.matching.size() + cat_filter.rejected.size() == data.size());
    std::cout << "[PASS] filter_by_category: " << cat_filter.matching.size()
              << " electronics, " << cat_filter.rejected.size() << " others\n";

    auto val_filter = filter_by_value_range(data, 100.0, 500.0);
    for (const auto& r : val_filter.matching) {
        assert(r.value >= 100.0 && r.value <= 500.0);
    }
    std::cout << "[PASS] filter_by_value_range: " << val_filter.matching.size()
              << " in [100, 500]\n";

    auto sorted_asc = sort_records_by_value(data, true);
    for (size_t i = 1; i < sorted_asc.size(); ++i) {
        assert(sorted_asc[i].value >= sorted_asc[i-1].value);
    }
    std::cout << "[PASS] sort_by_value ascending verified\n";

    auto sorted_name = sort_records_by_name(data);
    for (size_t i = 1; i < sorted_name.size(); ++i) {
        assert(sorted_name[i].name >= sorted_name[i-1].name);
    }
    std::cout << "[PASS] sort_by_name verified\n";

    auto summaries = compute_category_summaries(data);
    int total_counted = 0;
    for (const auto& [cat, s] : summaries) {
        assert(s.count > 0);
        assert(s.min_val <= s.average && s.average <= s.max_val);
        total_counted += s.count;
    }
    assert(total_counted == static_cast<int>(data.size()));
    std::cout << "[PASS] category summaries: " << summaries.size() << " categories\n";

    auto overall = compute_overall_summary(data);
    assert(overall.count == static_cast<int>(data.size()));
    std::cout << "[PASS] overall summary: avg=" << std::fixed << std::setprecision(2)
              << overall.average << "\n";

    auto data2 = generate_test_data(20);
    auto merged = merge_records(data, data2);
    assert(merged.size() == data.size() + data2.size());
    std::cout << "[PASS] merge_records: " << merged.size() << " total\n";

    auto deduped = deduplicate_records(merged);
    assert(deduped.size() <= merged.size());
    std::cout << "[PASS] deduplicate: " << deduped.size() << " unique from " << merged.size() << "\n";

    auto dupes = find_duplicates(merged);
    std::cout << "[PASS] find_duplicates: " << dupes.size() << " duplicate pairs\n";

    std::cout << "\n" << format_summary_table(summaries) << "\n";

    std::cout << "Histogram:\n" << generate_histogram(data, 8) << "\n";

    std::cout << generate_report(data);

    std::cout << "\nAll tests passed!\n";
}

int main() {
    run_all_tests();
    return 0;
}
