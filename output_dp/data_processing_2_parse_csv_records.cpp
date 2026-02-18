// Function: std::vector<Record> parse_csv_records(const std::string &)
// Source: example/data_processing.cpp (lines 42-68)
// ---

#include "data_processing_preamble.h"

#line 42 "/home/runner/workspace/example/data_processing.cpp"
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
