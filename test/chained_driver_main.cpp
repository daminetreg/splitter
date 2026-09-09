// Split while chained behind another compiler launcher. TODO/34.
//
// <string> is the point of this file. When the splitter probes the wrong program for the
// system include paths it gets none, and a translation unit that needs a standard header no
// longer parses the way the compiler sees it. Everything here is otherwise ordinary: two
// free functions so the unit really splits, and a value the program prints so a wrong parse
// cannot pass silently.
#include <string>
#include <cstdio>

std::string chained_greeting()
{
    return std::string("chained");
}

int chained_len()
{
    return static_cast<int>(chained_greeting().size());
}

int main() {
    std::printf("%s %d\n", chained_greeting().c_str(), chained_len());
    return 0;
}
