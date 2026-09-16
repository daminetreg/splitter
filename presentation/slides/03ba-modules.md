---
chapter: splitter waltkthrough
chapter-label: Modules
notes: Modules do a great job at not reparsing headers all the time — an importer reads the BMI instead of the header text. But the BMI is what every importer depends on, and even with -fmodules-reduced-bmi, which drops the non-inline bodies from it, clang stores the ODR hash of every definition in the BMI, so any body edit — a non-inline body too — changes the BMI and every importer rebuilds or misses the cache. Measured in TODO/43 with clang 21. Therefore let's autosplit them before we or LLMs adopt them: we, or the agent, will write everything in the same file, all lazy.
---
## {violet}modules{/violet} are the same

::: code-columns
```cpp
// foo.cxx — the interface unit
module;
#include <iostream>

export module foo;

export class foo {
public:
  foo();
  ~foo();
  void helloworld();
};
```
---
```cpp
// …and its bodies, in the same file
foo::foo() = default;
foo::~foo() = default;
void foo::helloworld() {
  std::cout << "hello world\n";
}

// every body edit changes the BMI:
// its ODR hash is stored, even with
// -fmodules-reduced-bmi
```
:::

::: rationale
No header reparsed by importers — but the BMI they read changes on every body edit, non-inline bodies included. So let's autosplit them before we, or the LLM, adopt them: we or the agent will write everything in the same file, all lazy.
:::
