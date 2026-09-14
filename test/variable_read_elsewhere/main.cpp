#include <cstdio>
#include "settings.h"

int unrelated(int v);

int main()
{
    std::printf("%d %d %d\n", Settings::tabsz, counter, unrelated(2));
    return Settings::tabsz == 2 && counter == 5 && unrelated(2) == 3 ? 0 : 1;
}
