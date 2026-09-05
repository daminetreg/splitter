#include "bench_common.hpp"

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
    std::cout << "  weight " << bench_weight(2) << "\n";
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

