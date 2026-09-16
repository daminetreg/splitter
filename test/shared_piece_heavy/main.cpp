#include <cstdio>
long use_a(int v);
long use_b(int v);
int main()
{
    std::printf("%ld %ld\n", use_a(2), use_b(2));
    return 0;
}
