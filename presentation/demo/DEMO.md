```
export TIPI_DISABLE_AR_RANLIB_DRIVER=ON TIPI_CACHE_CONSUME_ONLY=ON TIPI_CACHE_FORCE_ENABLE=OFF
cmake-re --build build/cmake-re-macos-apple-clang --host -j8
cmake-re --host -S . -B build/cmake-re-macos-brew-llvm -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=environments/macos-brew-llvm.cmake
cmake-re --build build/cmake-re-macos-brew-llvm --host -j8
```


```
cd /Users/daminetreg/workspace/splitter-talk/splitter-local/presentation/demo/mylib

rm -rf .cpp-splitter-store
rm -rf *.o*

# Plain
time clang++ -I. use_mylib.cpp

# Cold Split
export CPP_SPLITTER_VERBOSE=1
time ../../../build/cmake-re-macos-brew-llvm/cpp-splitter clang++ -I. -MD -MF use_mylib.o.d -c -o use_mylib.o use_mylib.cpp && clang++ use_mylib.o

# Do an edit
time ../../../build/cmake-re-macos-brew-llvm/cpp-splitter clang++ -I. -MD -MF use_mylib.o.d -c -o use_mylib.o use_mylib.cpp && clang++ use_mylib.o
```