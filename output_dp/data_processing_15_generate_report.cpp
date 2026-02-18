// Function: std::string generate_report(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 241-260)
// ---

#include "data_processing_preamble.h"

#line 241 "/home/runner/workspace/example/data_processing.cpp"
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
