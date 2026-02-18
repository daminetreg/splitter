// Function: void run_all_tests()
// Source: example/data_processing.cpp (lines 298-369)
// ---

#include "data_processing_preamble.h"

#line 298 "/home/runner/workspace/example/data_processing.cpp"
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
