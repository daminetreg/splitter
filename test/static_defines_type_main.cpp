// Regression fixture for TODO/42 (2): a static variable whose declaration also defines its
// type, `static struct T { ... } v = {...};`, as OpenCV's color_lab.cpp declares LABLUVLUTs16.
// The declaration that defines a type takes only the declarator out and leaves an `extern`
// in its place; a static is renamed so the pieces can share it. The two rules met here and
// the `extern` kept the original name while every use had the mangled one:
// `use of undeclared identifier '__static_color_lab_cpp__LABLUVLUTs16'`.
#include <cstdio>

static struct LUTs_t {
    const int* table;
    int size;
} LUTs = {nullptr, 0};

static const int kTable[4] = {10, 20, 30, 40};

void init_luts() { LUTs.table = kTable; LUTs.size = 4; }
int lut_sum() { int s = 0; for (int i = 0; i < LUTs.size; ++i) s += LUTs.table[i]; return s; }

int main()
{
    init_luts();
    if (lut_sum() != 100) return 1;
    std::printf("%d\n", lut_sum());
    return 0;
}
