#include "bench_common.hpp"

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
    std::cout << "  weight " << bench_weight(4) << "\n";
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

