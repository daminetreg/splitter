set(CMAKE_C_COMPILER /usr/local/share/.tipi/clang/4f846ee/bin/clang CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER /usr/local/share/.tipi/clang/4f846ee/bin/clang++ CACHE PATH "" FORCE)

# C++17 is the default for everything built through this toolchain, and the splitter depends on
# it rather than merely preferring it.
#
# An `inline` variable is the only way to leave a namespace-scope definition in a header that
# every split piece includes: without it the definition has to be moved to the definitions
# header, and a variable whose type cannot survive that move -- an array whose bound comes from
# its initialiser, a constant a constant-expression needs -- has no correct placement at all.
# prepare_variables() therefore keeps a variable in place and marks it `inline` from C++17 on,
# and warns once per unit when an older standard forces the move.
#
# The driver's own default is gnu++14 (see cached_driver_standard()), so leaving this unset
# means the splitter parses and places against C++14 rules and those defects come back.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
