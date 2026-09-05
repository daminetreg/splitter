#include "bench_common.hpp"

double evaluate_expression(const std::string& input) {
    double result = 0;
    std::string::const_iterator it = input.begin();
    std::string::const_iterator end = input.end();

    qi::rule<std::string::const_iterator, double(), ascii::space_type> expr, term, factor, base;

    base = qi::double_
         | ('(' >> expr >> ')');

    factor = base[qi::_val = qi::_1]
             >> *('^' >> base[qi::_val = phoenix::bind(
                 [](double a, double b) { return std::pow(a, b); },
                 qi::_val, qi::_1)]);

    term = factor[qi::_val = qi::_1]
           >> *(('*' >> factor[qi::_val *= qi::_1])
              | ('/' >> factor[qi::_val /= qi::_1])
              | ('%' >> factor[qi::_val = phoenix::bind(
                  [](double a, double b) { return std::fmod(a, b); },
                  qi::_val, qi::_1)]));

    expr = term[qi::_val = qi::_1]
           >> *(('+' >> term[qi::_val += qi::_1])
              | ('-' >> term[qi::_val -= qi::_1]));

    bool ok = qi::phrase_parse(it, end, expr, ascii::space, result);
    if (!ok || it != end) return std::numeric_limits<double>::quiet_NaN();
    return result;
}

std::string format_expression_result(const std::string& expr, double value) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    if (value == static_cast<int>(value)) {
        karma::generate(sink,
            karma::string << karma::lit(" = ") << karma::int_,
            expr, static_cast<int>(value)
        );
    } else {
        karma::generate(sink,
            karma::string << karma::lit(" = ") << karma::double_,
            expr, value
        );
    }

    return output;
}

std::string generate_expression_table(const std::vector<std::pair<std::string, double>>& entries) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    karma::generate(sink, karma::string,
        std::string("+-----------------------+------------------+\n"));
    karma::generate(sink, karma::string,
        std::string("| Expression            | Result           |\n"));
    karma::generate(sink, karma::string,
        std::string("+-----------------------+------------------+\n"));

    for (const auto& entry : entries) {
        std::ostringstream oss;
        if (entry.second == static_cast<int>(entry.second))
            oss << static_cast<int>(entry.second);
        else
            oss << entry.second;

        std::string expr_padded = entry.first;
        std::string val_padded = oss.str();
        while (expr_padded.size() < 21) expr_padded += ' ';
        while (val_padded.size() < 16) val_padded += ' ';

        karma::generate(sink,
            karma::lit("| ") << karma::string << karma::lit(" | ") << karma::string << karma::lit(" |\n"),
            expr_padded, val_padded
        );
    }

    karma::generate(sink, karma::string,
        std::string("+-----------------------+------------------+\n"));

    return output;
}

void run_expression_tests() {
    std::cout << "  weight " << bench_weight(3) << "\n";
    std::cout << "[Arithmetic Expressions]\n";

    std::vector<std::pair<std::string, double>> results;
    auto eval = [&](const std::string& expr) {
        double val = evaluate_expression(expr);
        results.push_back({expr, val});
        std::cout << "  " << format_expression_result(expr, val) << "\n";
    };

    eval("2 + 3");
    eval("10 - 4 * 2");
    eval("(10 - 4) * 2");
    eval("3.14 * 2");
    eval("100 / 3");
    eval("2 ^ 10");
    eval("(1 + 2) * (3 + 4)");
    eval("10 % 3");

    std::cout << "\n  Formatted table:\n";
    std::string table = generate_expression_table(results);
    std::istringstream iss(table);
    std::string line;
    while (std::getline(iss, line))
        std::cout << "    " << line << "\n";
    std::cout << "\n";
}

