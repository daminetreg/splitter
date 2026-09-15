// An importer: an ordinary translation unit whose pieces restate the import.
import math;
#include <cstdio>
int describe() { return twice(3) + seven(); }
int main() { std::printf("%d %d %d\n", twice(21), seven(), plus_one(describe())); return 0; }
