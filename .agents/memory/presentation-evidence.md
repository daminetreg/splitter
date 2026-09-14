---
name: Presentation evidence provenance
description: Avoid conflating architecture documentation with the prebuilt splitter used for binary measurements.
---
Treat the checked-in executable and the architecture document as potentially different implementations until their emitted trees agree.

**Why:** Presentation measurements found a prebuilt executable splitting main and using GCC inline-retention flags, while the architecture post describes a kept definitions owner and used attributes. A focused application-symbol diff also hid substantial retained library code.

**How to apply:** Label measured toolchain and executable behavior separately from documented architecture; qualify filtered nm diffs and show whole-binary size evidence before making layout claims.

The binary-impact report's detailed section table takes precedence over its broad “every loaded section” conclusion.

**Why:** Its LTO-plus-GC comparison reports equal code sections but different dynamic-symbol and relocation sections; equal stripped sizes also do not mean equal files.

**How to apply:** Distinguish byte-identical code sections, normalized instruction equality, equal size, and whole-file equality in presentation claims. Never generalize the first three into the fourth.