---
chapter: Binary evidence
chapter-label: Reasons for binary equality
notes: A semantic trace, not assembly or raw nm. LTO at ld.lld -r sees all pieces of the unit, restores inlining, then final --gc-sections drops uncalled weak copies.
---
## Inlined after   
{split}split.{/split}

::: binary-trace
{
  "main": {
    "label": "main",
    "source": "int main() { add(...); multiply(...); average(vals); greet(...); }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: one TU sees bodies → add/multiply/average fold into main.",
    "split": "split: definitions owner holds main; ld.lld -r ThinLTO sees all pieces → same folded main.",
    "note": "Measured outcome: 178 normalized instructions and 3,314 B .text in each executable."
  },
  "greet": {
    "label": "greet",
    "source": "inline std::string greet(...) { … }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: weak out-of-line greet remains.",
    "split": "split: greet is weak in both; no separate semantic difference.",
    "note": "Semantic trace of the measured mechanism; not raw assembly or an exact symbol dump."
  },
  "add": {
    "label": "add",
    "source": "inline int add(int a,int b) { return a+b; }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: folded into main; no standalone final copy.",
    "split": "split: piece emits a used weak copy; relink inlines it, then final --gc-sections removes the uncalled section.",
    "note": "Semantic trace of the measured mechanism; not raw assembly or an exact symbol dump."
  },
  "multiply": {
    "label": "multiply",
    "source": "inline int multiply(int a,int b) { return a*b; }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: folded into main; no standalone final copy.",
    "split": "split: relink restores folding; final section GC removes uncalled weak copy.",
    "note": "Semantic trace of the measured mechanism; not raw assembly or an exact symbol dump."
  },
  "average": {
    "label": "average",
    "source": "inline double average(vector<double> const&) { … }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: folded into main; vector construction folds away.",
    "split": "split: per-TU relink LTO restores the same fold; final GC removes uncalled weak copy.",
    "note": "Semantic trace of the measured mechanism; not raw assembly or an exact symbol dump."
  },
  "max_of": {
    "label": "max_of<T>",
    "source": "template<class T> T max_of(T a,T b) { … }",
    "plainLabel": "PLAIN-GC",
    "splitLabel": "SPLIT-LTO-RELINK-GC",
    "plain": "plain-gc: template stays in the header.",
    "split": "split: template stays in the header — no moved piece.",
    "note": "Semantic trace of the measured mechanism; not raw assembly or an exact symbol dump."
  }
}
:::

::: tiny
Summary: {accent}Identical executable code. Different symbol metadata.{/accent} Not byte-identical executables.
:::