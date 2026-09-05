// Shared header for the Spirit incremental benchmark.
//
// Everything expensive lives here: the Qi, Karma and Phoenix includes that make a Spirit
// translation unit what it is. Every unit includes it, so an edit to it is the case a
// splitter has to earn its keep on -- without splitting, touching this rebuilds every unit
// from scratch, and each of those takes seconds.
//
// `bench_weight()` is the body-edit target. It is an ordinary inline function in a widely
// included header: the shape whose body a developer changes twenty times an hour, and the
// shape the whole design is aimed at.
#pragma once

#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/karma.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/std_pair.hpp>
#include <boost/variant.hpp>
#include <boost/optional.hpp>
#include <iostream>
#include <map>
#include <numeric>
#include <cassert>
#include <sstream>

namespace qi = boost::spirit::qi;
namespace karma = boost::spirit::karma;
namespace phoenix = boost::phoenix;
namespace ascii = boost::spirit::ascii;

typedef boost::variant<double, std::string> ExprValue;


// The declarations the units share, carved from spirit_example.h.


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


// Body-edit target: an inline function in a widely included header.
inline unsigned int bench_weight(unsigned int n) {
    return n + 1;
}
