---
chapter: Architecture
chapter-label: 06 / Context survives
notes: Section 6 — members are emitted out-of-class using trailing return types; enclosing conditional context is replayed.
---
## Members move {violet}out{/violet}.  
Conditionals move {split}with{/split} them.

::: code-columns
```cpp
// class
int value() const { return value_; }

// rewritten declaration
int value() const;
```
---
```cpp
// piece
namespace demo {
auto Widget::value() const
  -> int { return value_; }
}
```
:::

::: rationale
Rationale — Re-emit member syntax where it is valid and replay active preprocessor conditionals to retain both lookup and availability.
:::