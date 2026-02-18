#pragma once
#line 1 "/home/runner/workspace/example/data_processing.cpp"
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

#line 40 "/home/runner/workspace/example/data_processing.cpp"


#line 68 "/home/runner/workspace/example/data_processing.cpp"


#line 79 "/home/runner/workspace/example/data_processing.cpp"


#line 93 "/home/runner/workspace/example/data_processing.cpp"


#line 107 "/home/runner/workspace/example/data_processing.cpp"


#line 115 "/home/runner/workspace/example/data_processing.cpp"


#line 123 "/home/runner/workspace/example/data_processing.cpp"


#line 143 "/home/runner/workspace/example/data_processing.cpp"


#line 163 "/home/runner/workspace/example/data_processing.cpp"


#line 175 "/home/runner/workspace/example/data_processing.cpp"


#line 197 "/home/runner/workspace/example/data_processing.cpp"


#line 211 "/home/runner/workspace/example/data_processing.cpp"


#line 223 "/home/runner/workspace/example/data_processing.cpp"


#line 239 "/home/runner/workspace/example/data_processing.cpp"


#line 260 "/home/runner/workspace/example/data_processing.cpp"


#line 296 "/home/runner/workspace/example/data_processing.cpp"


#line 369 "/home/runner/workspace/example/data_processing.cpp"


#line 374 "/home/runner/workspace/example/data_processing.cpp"


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
int main();
