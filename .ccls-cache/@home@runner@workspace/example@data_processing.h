#pragma once
#include <string>
#include <vector>
#include <map>
#include <utility>

struct Record {
    int id;
    std::string name;
    double value;
    std::string category;
};

struct Summary {
    std::string category;
    double total;
    double average;
    int count;
    double min_val;
    double max_val;
};

struct FilterResult {
    std::vector<Record> matching;
    std::vector<Record> rejected;
    int total_processed;
};

std::vector<Record> generate_test_data(int count);
std::vector<Record> parse_csv_records(const std::string& csv_data);
std::string records_to_csv(const std::vector<Record>& records);

FilterResult filter_by_category(const std::vector<Record>& records,
                                 const std::string& category);
FilterResult filter_by_value_range(const std::vector<Record>& records,
                                    double min_val, double max_val);
std::vector<Record> sort_records_by_value(std::vector<Record> records, bool ascending);
std::vector<Record> sort_records_by_name(std::vector<Record> records);

std::map<std::string, Summary> compute_category_summaries(const std::vector<Record>& records);
Summary compute_overall_summary(const std::vector<Record>& records);
std::string format_summary(const Summary& s);
std::string format_summary_table(const std::map<std::string, Summary>& summaries);

std::vector<Record> merge_records(const std::vector<Record>& a,
                                   const std::vector<Record>& b);
std::vector<Record> deduplicate_records(const std::vector<Record>& records);
std::vector<std::pair<Record, Record>> find_duplicates(const std::vector<Record>& records);

std::string generate_report(const std::vector<Record>& records);
std::string generate_histogram(const std::vector<Record>& records, int buckets);
void run_all_tests();
