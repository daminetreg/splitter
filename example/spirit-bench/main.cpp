#include "bench_common.hpp"

int main() {
    std::cout << "=== Boost.Spirit Qi/Karma Demo ===\n\n";
    run_roman_tests();
    run_xml_tests();
    run_expression_tests();
    run_markdown_tests();
    std::cout << "=== All tests complete ===\n";
    return 0;
}
