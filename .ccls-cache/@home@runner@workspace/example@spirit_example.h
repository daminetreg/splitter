#pragma once
#include <string>
#include <vector>
#include <utility>

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
