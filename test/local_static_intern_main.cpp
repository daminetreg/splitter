// A function in an unnamed namespace that interns through a function-local static, as p4c's
// lib/cstring.cpp does with `auto &cache()`. Kept in the preamble -- an unnamed-namespace
// function is -- it is one function per piece, and so is the cache: two pieces interning
// the same text got two addresses, and cstrings compared by pointer stopped comparing
// equal. The splitter declines the unit; the program below prints 1 only with one cache.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {
const char *intern(const char *s)
{
    static std::set<std::string> cache;
    return cache.insert(s).first->c_str();
}
}  // namespace

const char *first() { return intern("shared"); }
const char *second() { return intern("shared"); }

int main()
{
    const bool same = first() == second();
    std::printf("%d\n", same ? 1 : 0);
    return same ? 0 : 1;
}
