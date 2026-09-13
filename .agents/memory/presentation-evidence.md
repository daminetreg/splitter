---
name: Presentation evidence provenance
description: Avoid conflating architecture documentation with the prebuilt splitter used for binary measurements.
---
Treat the checked-in executable and the architecture document as potentially different implementations until their emitted trees agree.

**Why:** Presentation measurements found a prebuilt executable splitting main and using GCC inline-retention flags, while the architecture post describes a kept definitions owner and used attributes. A focused application-symbol diff also hid substantial retained library code.

**How to apply:** Label measured toolchain and executable behavior separately from documented architecture; qualify filtered nm diffs and show whole-binary size evidence before making layout claims.