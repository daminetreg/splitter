# The module benchmark project

One interface unit, `mathlib.cppm`, with 40 exported non-inline functions and 4 inline ones;
30 importers; a `main.cpp`. Written by `generate.py` and committed as generated, so that
`benchmark-cpp20-modules.sh` can edit a file the build watches. TODO/43 Phase 3.

```sh
./benchmark-cpp20-modules.sh 2          # plain against split, twice; needs Homebrew's llvm
```

Results and their reading: `benchmarks/cpp20-modules.md`.
