// Function: std::string records_to_csv(const std::vector<Record> &)
// Source: example/data_processing.cpp (lines 70-79)
// ---

#include "data_processing_preamble.h"

#line 70 "/home/runner/workspace/example/data_processing.cpp"
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
