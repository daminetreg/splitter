#pragma once
#include <string>
#include <vector>

// A registry filled during static initialisation, and a name it is filled with.
//
// This is the shape Boost.Test's runtime parameters have: `btrt_color_output` is a
// namespace-scope std::string with a dynamic initialiser, and the code that registers the
// parameter reads it while static initialisation is still running.
struct Registry {
    std::vector<std::string> names;
    void add(std::string const& n);
    bool has(std::string const& n) const;
};

Registry& registry();

// External linkage, not inline, not constexpr: exactly the shape the splitter moves into the
// definitions header, which is compiled into one piece rather than left in the preamble.
extern std::string param_name;

// Its constructor runs at static-initialisation time and reads param_name. If the two end up
// in different objects, the order between them is link order, not source order.
struct Registrar {
    Registrar();
};
