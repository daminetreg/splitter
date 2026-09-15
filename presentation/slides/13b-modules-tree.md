---
chapter: Architecture
chapter-label: 08 ½ / Modules · expected result
notes: What the launcher is expected to write for the interface unit — the same shape as for a .cpp, with the module's own outputs. CMake hands the module flags through the modmap response file. The interface the compiler precompiles is the rewritten one, declarations only, so foo.pcm does not change when a body does. Each moved body becomes an implementation unit of module foo, compiled against that BMI, and ld -r gives the build the foo.cxx.o it asked for. The BMI is written to a temporary path and only replaces the planned one when it differs, so nothing downstream sees a new digest.
---
## The build asks for {split}foo.cxx.o{/split}  
and {violet}foo.pcm{/violet}.

```cpp
cpp-splitter clang++ -std=c++20 @foo.cxx.o.modmap -c -o foo.cxx.o foo.cxx
// foo.cxx.o.modmap:  -x c++-module -fmodule-output=foo.pcm -fmodules-reduced-bmi
```

::: tree
foo.cxx.o.split/  
├── {violet}foo_interface.cxx{/violet} · declarations only → {violet}foo.pcm{/violet} + foo_interface.o  
├── {accent}foo_preamble.h{/accent} · the global module fragment, replayed by every piece  
├── foo.cxx_1_foo.cpp · foo.cxx_2_~foo.cpp · foo.cxx_3_helloworld.cpp  
│   · one implementation unit per body, compiled with -fmodule-file=foo=foo.pcm  
├── cache · hash · harvest · keeps · depfile (names foo.cxx and foo.pcm)  
└── *.o · ld -r → foo.cxx.o
:::
