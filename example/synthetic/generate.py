#!/usr/bin/env python3
"""Writes a CMake project of UNITS translation units with FUNCS free functions each: code
the splitter moves entirely into pieces. Deterministic. TODO/52.

    generate.py UNITS FUNCS DIR
"""
import os
import sys

units, funcs, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)


def write(name, text):
    with open(os.path.join(out, name), "w") as f:
        f.write(text)


write("synthetic.h", """#pragma once
// What every unit reads: the standard headers the bodies use, and inline helpers that
// every function calls, so the split build has shared pieces to link.
#include <algorithm>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

inline int mix(int a, int b) { return (a * 31 + b) % 1000003; }
inline std::string tag(int v) { return "v" + std::to_string(v); }
""")

for u in range(units):
    decls = "\n".join("int unit_%d_fn_%d(int x);" % (u, f) for f in range(funcs))
    write("unit_%d.h" % u, "#pragma once\n%s\n" % decls)
    bodies = []
    for f in range(funcs):
        bodies.append("""int unit_%(u)d_fn_%(f)d(int x)
{
    std::vector<int> v;
    for (int i = 0; i < x %% %(m)d + 3; ++i) v.push_back(mix(i, %(k)d));
    std::map<std::string, int> m;
    m[tag(x)] = static_cast<int>(v.size());
    std::sort(v.begin(), v.end());
    std::ostringstream os;
    os << x << ':' << m.size() << ':' << v.front();
    return static_cast<int>(os.str().size()) + std::accumulate(v.begin(), v.end(), 0) + %(f)d;
}
""" % {"u": u, "f": f, "m": 5 + (u + f) % 13, "k": (u * 7 + f * 3) % 101})
    write("unit_%d.cpp" % u, '#include "synthetic.h"\n#include "unit_%d.h"\n\n%s' % (u, "\n".join(bodies)))

includes = "\n".join('#include "unit_%d.h"' % u for u in range(units))
calls = "\n".join("    total += unit_%d_fn_%d(%d);" % (u, u % funcs, u) for u in range(units))
write("main.cpp", """#include <cstdio>
%s

int main()
{
    long total = 0;
%s
    std::printf("%%ld\\n", total);
    return 0;
}
""" % (includes, calls))

sources = " ".join("unit_%d.cpp" % u for u in range(units))
write("CMakeLists.txt", """cmake_minimum_required(VERSION 3.20)
project(synthetic CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(synthetic main.cpp %s)
""" % sources)
print("wrote %d units of %d functions to %s" % (units, funcs, out))
