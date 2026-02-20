#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/karma.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/std_pair.hpp>
#include <boost/variant.hpp>
#include <boost/optional.hpp>
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <map>
#include <numeric>
#include <cassert>
#include <sstream>

namespace qi = boost::spirit::qi;
namespace karma = boost::spirit::karma;
namespace phoenix = boost::phoenix;
namespace ascii = boost::spirit::ascii;



struct XmlAttribute {
    std::string name;
    std::string value;
};

struct XmlElement {
    std::string tag;
    std::vector<XmlAttribute> attributes;
    std::string text_content;
};

struct MarkdownSpan {
    std::string type;
    std::string content;
};

unsigned int parse_roman_numerals(const std::string& input);
std::string generate_roman_numeral(unsigned int value);
bool test_roman_roundtrip(const std::string& roman, unsigned int expected);
void run_roman_tests();

std::vector<XmlElement> parse_xml_elements(const std::string& input);
std::string generate_xml_element(const XmlElement& elem);
std::string generate_xml_document(const std::vector<XmlElement>& elements);
void run_xml_tests();

double evaluate_expression(const std::string& input);
std::string format_expression_result(const std::string& expr, double value);
std::string generate_expression_table(const std::vector<std::pair<std::string, double>>& entries);
void run_expression_tests();

std::vector<MarkdownSpan> parse_markdown_inline(const std::string& input);
std::string generate_markdown_html(const std::vector<MarkdownSpan>& spans);
std::string generate_markdown_summary(const std::vector<MarkdownSpan>& spans);
void run_markdown_tests();



typedef boost::variant<double, std::string> ExprValue;

unsigned int parse_roman_numerals(const std::string& input) {
    unsigned int result = 0;
    std::string::const_iterator it = input.begin();
    std::string::const_iterator end = input.end();

    qi::symbols<char, unsigned int> roman_hundreds;
    roman_hundreds.add
        ("C", 100)("CC", 200)("CCC", 300)("CD", 400)
        ("D", 500)("DC", 600)("DCC", 700)("DCCC", 800)("CM", 900);

    qi::symbols<char, unsigned int> roman_tens;
    roman_tens.add
        ("X", 10)("XX", 20)("XXX", 30)("XL", 40)
        ("L", 50)("LX", 60)("LXX", 70)("LXXX", 80)("XC", 90);

    qi::symbols<char, unsigned int> roman_ones;
    roman_ones.add
        ("I", 1)("II", 2)("III", 3)("IV", 4)
        ("V", 5)("VI", 6)("VII", 7)("VIII", 8)("IX", 9);

    qi::symbols<char, unsigned int> roman_thousands;
    roman_thousands.add
        ("M", 1000)("MM", 2000)("MMM", 3000);

    bool ok = qi::parse(it, end,
        -roman_thousands[phoenix::ref(result) += qi::_1]
        >> -roman_hundreds[phoenix::ref(result) += qi::_1]
        >> -roman_tens[phoenix::ref(result) += qi::_1]
        >> -roman_ones[phoenix::ref(result) += qi::_1]
    );

    if (!ok || it != end) return 0;
    return result;
}

std::string generate_roman_numeral(unsigned int value) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    qi::symbols<unsigned int, const char*> thousands_map;
    unsigned int thousands = value / 1000;
    value %= 1000;
    unsigned int hundreds = value / 100;
    value %= 100;
    unsigned int tens = value / 10;
    unsigned int ones = value % 10;

    const char* thousands_table[] = {"", "M", "MM", "MMM"};
    const char* hundreds_table[] = {"", "C", "CC", "CCC", "CD", "D", "DC", "DCC", "DCCC", "CM"};
    const char* tens_table[] = {"", "X", "XX", "XXX", "XL", "L", "LX", "LXX", "LXXX", "XC"};
    const char* ones_table[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};

    karma::generate(sink,
        karma::string << karma::string << karma::string << karma::string,
        std::string(thousands_table[thousands]),
        std::string(hundreds_table[hundreds]),
        std::string(tens_table[tens]),
        std::string(ones_table[ones])
    );

    return output;
}

bool test_roman_roundtrip(const std::string& roman, unsigned int expected) {
    unsigned int parsed = parse_roman_numerals(roman);
    if (parsed != expected) {
        std::cerr << "  FAIL parse: " << roman << " -> " << parsed << " (expected " << expected << ")\n";
        return false;
    }
    std::string generated = generate_roman_numeral(expected);
    if (generated != roman) {
        std::cerr << "  FAIL generate: " << expected << " -> " << generated << " (expected " << roman << ")\n";
        return false;
    }
    return true;
}

void run_roman_tests() {
    std::cout << "[Roman Numerals]\n";
    int pass = 0, fail = 0;
    auto check = [&](const std::string& r, unsigned int v) {
        if (test_roman_roundtrip(r, v)) ++pass; else ++fail;
    };
    check("I", 1);
    check("IV", 4);
    check("IX", 9);
    check("XIV", 14);
    check("XLII", 42);
    check("XCIX", 99);
    check("CDXLIV", 444);
    check("DCCCXC", 890);
    check("MCMXCIX", 1999);
    check("MMMCMXCIX", 3999);
    check("MMXXVI", 2026);
    std::cout << "  " << pass << " passed, " << fail << " failed\n\n";
}

std::vector<XmlElement> parse_xml_elements(const std::string& input) {
    std::vector<XmlElement> elements;
    std::string::const_iterator it = input.begin();
    std::string::const_iterator end = input.end();

    while (it != end) {
        XmlElement elem;

        if (!qi::parse(it, end, qi::lit('<')))
            { ++it; continue; }

        if (!qi::parse(it, end,
            qi::as_string[+(qi::char_ - '>' - qi::space - '/')],
            elem.tag))
            continue;

        while (it != end && *it != '>') {
            auto saved = it;
            qi::parse(it, end, +qi::space);
            if (it == end || *it == '>' || *it == '/') { it = saved; break; }

            std::string aname, aval;
            bool got_name = qi::parse(it, end,
                qi::as_string[+(qi::char_ - '=' - '>' - qi::space)],
                aname);
            if (!got_name) { it = saved; break; }

            bool got_eq = qi::parse(it, end, qi::lit('='));
            if (!got_eq) { it = saved; break; }

            if (!qi::parse(it, end, qi::lit('"'))) { it = saved; break; }
            bool got_val = qi::parse(it, end,
                qi::as_string[*(qi::char_ - '"')],
                aval);
            if (!got_val) { it = saved; break; }
            qi::parse(it, end, qi::lit('"'));

            elem.attributes.push_back({aname, aval});
        }

        qi::parse(it, end, qi::lit('>'));

        qi::parse(it, end,
            qi::as_string[*(qi::char_ - '<')],
            elem.text_content
        );

        qi::parse(it, end,
            qi::lit("</") >> *(qi::char_ - '>') >> qi::lit('>')
        );

        elements.push_back(elem);
    }

    return elements;
}

std::string generate_xml_element(const XmlElement& elem) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    if (elem.attributes.empty()) {
        karma::generate(sink,
            karma::lit('<') << karma::string << karma::lit('>')
            << karma::string
            << karma::lit("</") << karma::string << karma::lit('>'),
            elem.tag, elem.text_content, elem.tag
        );
    } else {
        karma::generate(sink,
            karma::lit('<') << karma::string,
            elem.tag
        );
        for (const auto& attr : elem.attributes) {
            karma::generate(sink,
                karma::lit(' ') << karma::string << karma::lit("=\"") << karma::string << karma::lit('"'),
                attr.name, attr.value
            );
        }
        karma::generate(sink,
            karma::lit('>') << karma::string << karma::lit("</") << karma::string << karma::lit('>'),
            elem.text_content, elem.tag
        );
    }

    return output;
}

std::string generate_xml_document(const std::vector<XmlElement>& elements) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    karma::generate(sink, karma::string, std::string("<?xml version=\"1.0\"?>\n<root>\n"));
    for (const auto& elem : elements) {
        karma::generate(sink,
            karma::string << karma::string << karma::string,
            std::string("  "), generate_xml_element(elem), std::string("\n")
        );
    }
    karma::generate(sink, karma::string, std::string("</root>"));

    return output;
}

void run_xml_tests() {
    std::cout << "[XML Parsing & Generation]\n";

    std::string xml_input =
        "<title>Hello World</title>"
        "<author name=\"John\">Some text</author>"
        "<item id=\"42\" class=\"special\">Content here</item>";

    auto elements = parse_xml_elements(xml_input);
    std::cout << "  Parsed " << elements.size() << " elements:\n";
    for (const auto& e : elements) {
        std::cout << "    <" << e.tag;
        for (const auto& a : e.attributes)
            std::cout << " " << a.name << "=\"" << a.value << "\"";
        std::cout << ">" << e.text_content << "</" << e.tag << ">\n";
    }

    std::string doc = generate_xml_document(elements);
    std::cout << "  Generated document:\n";
    std::istringstream iss(doc);
    std::string line;
    while (std::getline(iss, line))
        std::cout << "    " << line << "\n";
    std::cout << "\n";
}

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

std::vector<MarkdownSpan> parse_markdown_inline(const std::string& input) {
    std::vector<MarkdownSpan> spans;
    std::string::const_iterator it = input.begin();
    std::string::const_iterator end = input.end();

    while (it != end) {
        std::string content;

        if (qi::parse(it, end, qi::lit("**"))) {
            if (qi::parse(it, end, qi::as_string[+(qi::char_ - '*')], content)) {
                qi::parse(it, end, qi::lit("**"));
                spans.push_back({"bold", content});
            }
            continue;
        }

        if (*it == '*') {
            auto saved = it;
            ++it;
            if (qi::parse(it, end, qi::as_string[+(qi::char_ - '*')], content)) {
                qi::parse(it, end, qi::lit('*'));
                spans.push_back({"italic", content});
            } else {
                it = saved;
                spans.push_back({"text", std::string(1, *it)});
                ++it;
            }
            continue;
        }

        if (*it == '`') {
            ++it;
            if (qi::parse(it, end, qi::as_string[+(qi::char_ - '`')], content)) {
                qi::parse(it, end, qi::lit('`'));
                spans.push_back({"code", content});
            }
            continue;
        }

        if (*it == '[') {
            ++it;
            std::string url;
            if (qi::parse(it, end, qi::as_string[+(qi::char_ - ']')], content)
                && qi::parse(it, end, qi::lit("]("))
                && qi::parse(it, end, qi::as_string[+(qi::char_ - ')')], url)
                && qi::parse(it, end, qi::lit(')'))) {
                spans.push_back({"link", content});
            }
            continue;
        }

        if (qi::parse(it, end, qi::as_string[+(qi::char_ - '*' - '`' - '[')], content)) {
            spans.push_back({"text", content});
            continue;
        }

        ++it;
    }

    return spans;
}

std::string generate_markdown_html(const std::vector<MarkdownSpan>& spans) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    for (const auto& span : spans) {
        if (span.type == "bold") {
            karma::generate(sink,
                karma::lit("<strong>") << karma::string << karma::lit("</strong>"),
                span.content
            );
        } else if (span.type == "italic") {
            karma::generate(sink,
                karma::lit("<em>") << karma::string << karma::lit("</em>"),
                span.content
            );
        } else if (span.type == "code") {
            karma::generate(sink,
                karma::lit("<code>") << karma::string << karma::lit("</code>"),
                span.content
            );
        } else if (span.type == "link") {
            karma::generate(sink,
                karma::lit("<a>") << karma::string << karma::lit("</a>"),
                span.content
            );
        } else {
            karma::generate(sink, karma::string, span.content);
        }
    }

    return output;
}

std::string generate_markdown_summary(const std::vector<MarkdownSpan>& spans) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    karma::generate(sink, karma::string, std::string("Spans: "));

    std::map<std::string, int> counts;
    for (const auto& s : spans) counts[s.type]++;

    bool first = true;
    for (const auto& kv : counts) {
        if (!first) {
            karma::generate(sink, karma::string, std::string(", "));
        }
        karma::generate(sink,
            karma::int_ << karma::lit(" ") << karma::string,
            kv.second, kv.first
        );
        first = false;
    }

    return output;
}

void run_markdown_tests() {
    std::cout << "[Markdown Parsing]\n";

    std::vector<std::string> inputs = {
        "Hello **world** and *universe*",
        "Use `code` for inline code",
        "Check [this link](https://example.com) out",
        "**Bold** and *italic* and `code` together",
        "A [link](http://test.com) with **bold text** here"
    };

    for (const auto& input : inputs) {
        std::cout << "  Input:   " << input << "\n";
        auto spans = parse_markdown_inline(input);
        std::string html = generate_markdown_html(spans);
        std::string summary = generate_markdown_summary(spans);
        std::cout << "  HTML:    " << html << "\n";
        std::cout << "  " << summary << "\n\n";
    }
}


std::string generate_some_other_markdown_html(const std::vector<MarkdownSpan>& spans) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    for (const auto& span : spans) {
        if (span.type == "bold") {
            karma::generate(sink,
                karma::lit("<bold>") << karma::string << karma::lit("</bold>"),
                span.content
            );
        } else if (span.type == "italic") {
            karma::generate(sink,
                karma::lit("<em>") << karma::string << karma::lit("</em>"),
                span.content
            );
        } else if (span.type == "code") {
            karma::generate(sink,
                karma::lit("<code>") << karma::string << karma::lit("</code>"),
                span.content
            );
        } else if (span.type == "link") {
            karma::generate(sink,
                karma::lit("<a>") << karma::string << karma::lit("</a>"),
                span.content
            );
        } else {
            karma::generate(sink, karma::string, span.content);
        }
    }

    return output;
}


std::string generate_some_anoother_markdown_html(const std::vector<MarkdownSpan>& spans) {
    std::string output;
    std::back_insert_iterator<std::string> sink(output);

    for (const auto& span : spans) {
        if (span.type == "bold") {
            karma::generate(sink,
                karma::lit("<bold>") << karma::string << karma::lit("</bold>"),
                span.content
            );
        } else if (span.type == "italic") {
            karma::generate(sink,
                karma::lit("<me>") << karma::string << karma::lit("</me>"),
                span.content
            );
        } else if (span.type == "code") {
            karma::generate(sink,
                karma::lit("<code>") << karma::string << karma::lit("</code>"),
                span.content
            );
        } else if (span.type == "link") {
            karma::generate(sink,
                karma::lit("<a>") << karma::string << karma::lit("</a>"),
                span.content
            );
        } else {
            karma::generate(sink, karma::string, span.content);
        }
    }

    return output;
}

int main() {
    std::cout << "=== Boost.Spirit Qi/Karma Demo ===\n\n";
    run_roman_tests();
    run_xml_tests();
    run_expression_tests();
    run_markdown_tests();
    std::cout << "=== All tests complete ===\n";
    generate_some_other_markdown_html();
    generate_some_anoother_markdown_html();
    return 0;
}

