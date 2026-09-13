# Binary symbol evidence: `use_mylib`

**Status: measured actual splitter run.** This evidence was produced from the checked-in
`test/mylib.h` and `test/use_mylib.cpp` without changing project source. The
machine-readable record is [symbol-evidence.json](./symbol-evidence.json).

## Toolchain and commands

`cpp-splitter` was available as the repository-relative executable `./cpp-splitter`
(it was not on `PATH`). The successful measurement used:

```text
g++ (GCC) 14.3.0
GNU nm 2.44
GNU size 2.44
GNU ld 2.44
```

For each optimization level, plain compilation was:

```sh
g++ -std=c++17 -O0 -c src/use_mylib.cpp -o plain/use_mylib_O0.o
g++ -std=c++17 -O0 plain/use_mylib_O0.o -o plain/use_mylib_O0
g++ -std=c++17 -O2 -c src/use_mylib.cpp -o plain/use_mylib_O2.o
g++ -std=c++17 -O2 plain/use_mylib_O2.o -o plain/use_mylib_O2
```

The actual splitter runs were:

```sh
CPP_SPLITTER_VERBOSE=1 CPP_SPLITTER_NO_LINKER_DRIVER=1 \
  ./cpp-splitter g++ -std=c++17 -O0 -Isrc -c \
  -o actual/use_mylib_O0.o src/use_mylib.cpp
g++ -std=c++17 -O0 actual/use_mylib_O0.o -o actual/use_mylib_O0

CPP_SPLITTER_VERBOSE=1 CPP_SPLITTER_NO_LINKER_DRIVER=1 \
  ./cpp-splitter g++ -std=c++17 -O2 -Isrc -c \
  -o actual/use_mylib_O2.o src/use_mylib.cpp
g++ -std=c++17 -O2 actual/use_mylib_O2.o -o actual/use_mylib_O2
```

The verbose splitter output confirms `ld -r` over one main piece and four header
pieces. The focused symbol command was:

```sh
nm -C --defined-only <file>
```

For set comparison, addresses were normalized with:

```sh
nm -C --defined-only <file> |
  sed -E 's/^[0-9a-f]+ /<addr> /' |
  grep -E 'add\(|multiply\(|greet\(|average\(|max_of| main$'
```

## Focused defined symbols

`T` is a text/global definition and `W` is a weak definition. A dash means the
focused symbol was not a defined symbol in that object or executable.

| function | plain O0 object | split O0 object | plain O0 exe | split O0 exe | plain O2 object | split O2 object | plain O2 exe | split O2 exe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `main` | T | T | T | T | T | T | T | T |
| `add(int,int)` | W | W | W | W | — | W | — | W |
| `multiply(int,int)` | W | W | W | W | — | W | — | W |
| `greet(string const&)` | W | W | W | W | — | W | — | W |
| `average(vector<double> const&)` | W | W | W | W | — | W | — | W |
| `int max_of<int>(int,int)` | W | W | W | W | — | — | — | — |

The names above are shortened only for table width. The JSON contains the exact
`nm -C` names, including the libstdc++ ABI spelling.

### Split piece ownership

This is the extra fact that an aggregate `nm` cannot show:

| optimization | generated object | focused defined symbols |
|---|---|---|
| `-O0` | `use_mylib_1_main.o` | `main (T)`, `int max_of<int>(int,int) (W)` |
| `-O0` | `mylib_1_add.o` | `add(int,int) (W)` |
| `-O0` | `mylib_2_multiply.o` | `multiply(int,int) (W)` |
| `-O0` | `mylib_3_greet.o` | `greet(string const&) (W)` |
| `-O0` | `mylib_4_average.o` | `average(vector<double> const&) (W)` |
| `-O2` | `use_mylib_1_main.o` | `main (T)` |
| `-O2` | `mylib_1_add.o` | `add(int,int) (W)` |
| `-O2` | `mylib_2_multiply.o` | `multiply(int,int) (W)` |
| `-O2` | `mylib_3_greet.o` | `greet(string const&) (W)` |
| `-O2` | `mylib_4_average.o` | `average(vector<double> const&) (W)` |

`max_of` remains in the rewritten header/template path and has no separately
compiled piece. At `-O0`, its `int` instantiation is emitted from the main piece.
At `-O2`, GCC inlines it and emits no focused `max_of` symbol.

## Normalized `nm -C` diffs

The normalized address-independent result for **both** the aggregate object and
the final executable at `-O0` is:

```text
common:
<addr> T main
<addr> W add(int, int)
<addr> W multiply(int, int)
<addr> W greet(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)
<addr> W average(std::vector<double, std::allocator<double> > const&)
<addr> W int max_of<int>(int, int)
plain-only: (none)
split-only: (none)
```

At `-O2`, the normalized result for both scopes is:

```text
common:
<addr> T main
plain-only: (none)
split-only:
<addr> W add(int, int)
<addr> W multiply(int, int)
<addr> W greet(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)
<addr> W average(std::vector<double, std::allocator<double> > const&)
```

The `-O2` difference is not a claim that splitting prevents all inlining. The
plain TU has no out-of-line focused inline definitions after optimization,
whereas the splitter's verbose compile commands explicitly add
`-fkeep-inline-functions` when compiling header pieces. `max_of` is a template
and remains absent from the focused `-O2` set.

## Measured sizes and layout

These are the exact `size` section totals; file bytes are included as a separate
`stat` measurement.

| optimization | artifact | file bytes | text | data | bss | dec |
|---|---|---:|---:|---:|---:|---:|
| `-O0` | plain object | 41,568 | 7,344 | 0 | 0 | 7,344 |
| `-O0` | split `ld -r` object | 387,744 | 58,465 | 0 | 0 | 58,465 |
| `-O0` | plain executable | 33,400 | 13,486 | 872 | 280 | 14,638 |
| `-O0` | split executable | 148,992 | 74,773 | 1,376 | 280 | 76,429 |
| `-O2` | plain object | 6,152 | 1,131 | 0 | 0 | 1,131 |
| `-O2` | split `ld -r` object | 313,624 | 35,943 | 0 | 0 | 35,943 |
| `-O2` | plain executable | 17,264 | 4,150 | 728 | 280 | 5,158 |
| `-O2` | split executable | 114,904 | 51,041 | 1,160 | 280 | 52,481 |

For this small fixture, split executable text is therefore 61,287 bytes larger
at `-O0` and 46,891 bytes larger at `-O2`. This is a layout/code-retention fact
from this invocation, not a general performance result: each header piece carries
header context, and GCC's `-fkeep-inline-functions` retains weak inline bodies.

## Generated source for inspection

The following is the generated source observed under
`actual/use_mylib_O0.o.split/` (the `-O2` generated source is text-identical).
Ephemeral `/tmp/...` prefixes in `#line` directives are normalized to `test/...`;
the bodies and structure are unchanged. The same content is available as
`generated_sources` in [symbol-evidence.json](./symbol-evidence.json).

<details>
<summary><code>use_mylib_preamble.h</code></summary>

```cpp
#pragma once
#line 1 "test/use_mylib.cpp"
#include "mylib.h"
#include <iostream>

#line 15 "test/use_mylib.cpp"


int main();
```
</details>

<details>
<summary><code>mylib.h</code> (rewritten mirror)</summary>

```cpp
#pragma once
#line 1 "test/mylib.h"
#pragma once
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

#line 9 "test/mylib.h"


#line 13 "test/mylib.h"


#line 17 "test/mylib.h"


#line 23 "test/mylib.h"


#line 25 "test/mylib.h"
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}
#line 28 "test/mylib.h"


int add(int a, int b);
int multiply(int a, int b);
std::string greet(const std::string& name);
double average(const std::vector<double>& values);
```
</details>

<details>
<summary><code>use_mylib_1_main.cpp</code></summary>

```cpp
// Function: int main()
// Source: test/use_mylib.cpp (lines 4-15)
// ---

#include "use_mylib_preamble.h"

#line 4 "test/use_mylib.cpp"
int main() {
    std::cout << "add(3, 4) = " << add(3, 4) << "\n";
    std::cout << "multiply(5, 6) = " << multiply(5, 6) << "\n";
    std::cout << greet("World") << "\n";

    std::vector<double> vals = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::cout << "average = " << average(vals) << "\n";

    std::cout << "max_of(10, 20) = " << max_of(10, 20) << "\n";

    return 0;
}
```
</details>

<details>
<summary>Header pieces: <code>mylib_1_add.cpp</code>, <code>mylib_2_multiply.cpp</code></summary>

```cpp
// Function: int add(int, int)
// Source: test/mylib.h (lines 7-9)
// ---

#include "mylib.h"

#line 7 "test/mylib.h"
inline int add(int a, int b) {
    return a + b;
}
```

```cpp
// Function: int multiply(int, int)
// Source: test/mylib.h (lines 11-13)
// ---

#include "mylib.h"

#line 11 "test/mylib.h"
inline int multiply(int a, int b) {
    return a * b;
}
```
</details>

<details>
<summary>Header pieces: <code>mylib_3_greet.cpp</code>, <code>mylib_4_average.cpp</code></summary>

```cpp
// Function: std::string greet(const std::string &)
// Source: test/mylib.h (lines 15-17)
// ---

#include "mylib.h"

#line 15 "test/mylib.h"
inline std::string greet(const std::string& name) {
    return "Hello, " + name + "!";
}
```

```cpp
// Function: double average(const std::vector<double> &)
// Source: test/mylib.h (lines 19-23)
// ---

#include "mylib.h"

#line 19 "test/mylib.h"
inline double average(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}
```
</details>

<details>
<summary><code>mylib_5_max_of.cpp</code> (template kept, not a linked object)</summary>

```cpp
// Function: T max_of(T, T)
// Source: test/mylib.h (lines 25-28)
// Note: template - kept in preamble header for compilation
// ---

#include "mylib.h"

#line 25 "test/mylib.h"
template<typename T>
T max_of(T a, T b) {
    return (a > b) ? a : b;
}
```
</details>

## Caveats for the talk

* This is actual `./cpp-splitter` output with GCC, not a manually reconstructed
  split. No installation, workflow run, or project-source edit was needed.
* The generated files from this executable differ slightly from the illustrative
  architecture excerpt: they show `#include "mylib.h"` directly and no visible
  `__attribute__((used))`. The verbose command shows GCC's
  `-fkeep-inline-functions`, which is the observed mechanism retaining the weak
  header definitions.
* `nm` addresses are deliberately removed from the diffs. Final addresses differ
  because the split executable has a different layout and much more retained
  header support code.
* `max_of` demonstrates template/optimization behavior rather than a stable
  “one function, one symbol” rule: it is emitted at `-O0` and inlined away at
  `-O2`.
* The first exploratory clang wrapper was not used in the reported measurements.
  The reported plain and split pairs are consistently GCC 14.3.0.