#include "bench_common.hpp"

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
    std::cout << "  weight " << bench_weight(1) << "\n";
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

