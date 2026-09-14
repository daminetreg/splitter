// Static variables of two shapes that used to stay in the preamble -- one copy per piece,
// and the link says nothing about it, since they have internal linkage. p4c's gc.cpp writes
// both: `static bool done_init, started_init;` and `static char emergency_pool[16 * 1024];`,
// and the split irgenerator initialised the collector once per copy and aborted (TODO/47).
// Both are renamed and moved like any other static; the program below prints wrong numbers
// if any piece has a copy of its own.
#include <cstdio>
#include <cstring>

static bool done_init, started_init;
static char pool[16 * 4];
static char *pool_ptr;

// An array of function pointers: the `(` after the type opens a declarator, not an
// initialiser. p4c's `static size_t (*mod_hashsize[])(size_t x) = {...}`.
static int twice(int v) { return v * 2; }
static int thrice(int v) { return v * 3; }
static int (*scalers[])(int v) = {twice, thrice};

// State in an unnamed namespace, as p4c's bison-generated ir-generator.cpp keeps
// `static IrNamespace *current_namespace` in one: moved out and renamed like a static, the
// namespace closed and reopened around the declaration left behind.
namespace {
int *current = nullptr;
int slots[4] = {10, 20, 30, 40};
}  // namespace

void init()
{
    if (!done_init) {
        started_init = true;
        std::memset(pool, 'x', sizeof(pool));
        pool_ptr = pool;
        done_init = true;
    }
}

int take(int n)
{
    if (!done_init || !started_init) return -1;
    pool_ptr += n;
    return static_cast<int>(pool_ptr - pool);
}

int pool_size() { return static_cast<int>(sizeof(pool)); }
int scaled(int which, int v) { return scalers[which](v); }
void select(int i) { current = &slots[i]; }
int selected() { return current ? *current : -1; }

int main()
{
    init();
    const int a = take(3), b = take(4);
    select(2);
    std::printf("%d %d %d %d %d\n", a, b, pool_size(), scaled(1, 5), selected());
    return a == 3 && b == 7 && pool_size() == 64 && scaled(1, 5) == 15 && selected() == 30 ? 0 : 1;
}
