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

When recovering presentation content from older revisions, use the final visible state, not the first matching data object or HTML section.

**Why:** The original deck retained hidden measurement slides and multiple later script overrides. Reading the first declaration recovered superseded measurements, and copying only initial CSS lost the final chart scale and compact layout.

**How to apply:** Check whether historical snippets were active and overridden before reusing them. The benchmark report and binary-impact report remain the evidence sources; old deck code is not an independent measurement.