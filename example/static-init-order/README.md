# A minimal reproduction, without Boost

This is the smallest program I could find that shows what goes wrong, and it needs neither
Boost.Geometry nor Boost.Test. It is here on the `conversion-operator-harvest` branch because
that is what led to it, but **it fails on `main` too** — see "What this is not" below.

## Run it

```sh
cmake -GNinja -S example/static-init-order -B example/static-init-order/tmp/sio \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/environments/monolithic.cmake
ninja -C example/static-init-order/tmp/sio && example/static-init-order/tmp/sio/static_init_order          # registered: 'color_output'  exit 0

cmake -GNinja -S example/static-init-order -B example/static-init-order/tmp/sio-split \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/environments/monolithic.cmake \
      -DCMAKE_CXX_COMPILER_LAUNCHER=$PWD/build/cpp-splitter
ninja -C example/static-init-order/tmp/sio-split && example/static-init-order/tmp/sio-split/static_init_order   # registered: ''  exit 1
```

Same source, same compiler, different answer. Nothing falls back and nothing fails to build:
the split program simply computes something else.

## The shape

`registry.cpp` has two things at namespace scope:

```cpp
std::string param_name = "color_output";   // external linkage, dynamic initialiser
static Registrar s_registrar;              // its constructor reads param_name
```

After splitting, they are in different places:

```
registry_preamble.h          static Registrar s_registrar;      <- included by EVERY piece
registry.cpp_definitions.h   std::string param_name = "...";    <- included by ONE piece
```

Two separate things have gone wrong, and the repro shows both at once.

* `param_name` was moved to the definitions header, so its dynamic initialiser now runs in a
  different object from the code that reads it. Within one translation unit the order is
  source order; across objects merged by `ld -r` it is link order. Nothing preserves it.

* `s_registrar` has internal linkage, so the preamble gives **every piece its own copy** and
  the registry is filled several times over. That is the residual recorded in `TODO/17`'s
  outcome — "a `static int calls = 0;` shared by two functions now counts in two objects" —
  seen doing damage rather than merely being untidy.

The first registration wins in the sense that matters, and it sees an empty string.

## It is not the Boost.Test failure — that was tested and it is not

The reason this file exists is that Boost.Test writes the same shape in
`boost/test/impl/unit_test_parameters.ipp`:

```cpp
namespace runtime_config {
std::string btrt_color_output = "color_output";      // namespace scope, dynamic initialiser
...
    rt::option color_output( btrt_color_output, ... );
    store.add( color_output );                        // reads it during setup
```

and the split Boost.Geometry test fails with

```
Test setup error: There is no argument provided for parameter color_output
```

which is what a parameter registered under an empty name looks like from the outside. That
made initialisation order the obvious suspect. **It is the wrong suspect.** Building the same
target at C++17, where the splitter now leaves those variables in place as `inline` variables
and moves nothing:

```
cold: exit=0   fallbacks=0   warnings=0
Test setup error: There is no argument provided for parameter color_output
program exit=200
```

Zero warnings means no variable was moved at all, so no initialiser changed object, so
initialisation order is not what breaks it. Same failure, same message.

The earlier revision of this file called it "the strongest available hypothesis, not
established". It was honest about being unproven and it was still wrong, which is the useful
part: the shapes matching and the symptom matching was never evidence, and one experiment that
could have falsified it was worth more than the argument for it.

What is still true is that the Boost.Geometry failure appears only with this branch's
conversion-operator harvest, and that keeping those definitions in the header while merely
*relocating* them reproduces it. That is where to look next, and it is not here.

## What this is not

It is **not** caused by the change this branch carries. The repro fails identically with the
conversion-operator harvest on and off:

| binary | result |
|---|---|
| `main` (harvest off) | `registered: ''`, exit 1 |
| this branch (harvest on) | `registered: ''`, exit 1 |

So the defect is on `main` today and predates the branch. What the harvest appears to do is
change *which piece carries what* — it adds definitions to the range list, which moves the
boundary between the piece that includes the definitions header and the rest — and thereby
flips Boost.Geometry's test from happening to work to happening to fail.

That reframes the branch: turning the harvest on does not introduce a bug, it perturbs an
existing one. Which is worth knowing before spending time on the conversion operators, because
fixing them will not make the Geometry test pass while the initialisation order is still
whatever the linker felt like.

## What the branch now does about it

The branch carries a fix for the ordering half: **from C++17 the definition stays where it
is, marked `inline`**, so the linker merges the copies and it is still initialised in source
order relative to its neighbours. Below C++17 inline variables do not exist, so it still moves
-- and the splitter now says so, once per translation unit and only when it actually moves
something:

```
[cpp-splitter] warning: registry.cpp: 'param_name' moved to another object
    (C++14; needs C++17 to stay put); its initialiser now runs in link order
    -- see example/static-init-order/
```

Measured on this example:

| standard | placement | first registration | exit |
|---|---|---|---:|
| C++14 | moved to the definitions header, warned | `''` | 1 |
| C++17 | `inline` in the preamble, silent | `'color_output'` | 0 |

The duplicate registrations your loop prints are *not* fixed by this. `s_registrar` has
internal linkage, so every piece still gets its own copy -- `TODO/17`'s recorded residual.
`inline` addresses where the variable lives, not how many registrars run.

`launcher.inline_variable_placement` pins both halves down. It asserts on the *placement* and
on the warning rather than on the program's answer, deliberately: a link whose order happens
to be favourable produces a correct program from wrong placement, so a runtime assertion here
passes with and without the fix -- which is exactly what the first version of that test did.

## What the branch itself does

One line in `visitor()`:

```cpp
kind == CXCursor_ConversionFunction ||
```

`collect_emitted()` has always counted conversion functions; the harvest never did. Without it
an out-of-line `operator T()` in a header is never moved out of the preamble, so every piece
gets a copy and `ld -r` rejects them — which is the last thing standing between Boost.Geometry's
tests and a cold build that splits:

```
multiple definition of `boost::test_tools::tt_detail::context_frame::operator bool()'
```

With the line in, that cold build splits with **no fallbacks at all**. Then the program fails
as above.

Two things to know before iterating on it:

* `FunctionInfo::is_conversion` forces `keep_in_header`, so the harvest cannot also start
  emitting pieces for them. `operator T()` names its type where a return type would go, and
  the out-of-line form rebuilt from `return_type + qualified_name` comes out as
  `bool C::operator bool()`. Relaxing that needs the declarator rebuilt differently.
* `sanitize_filename()` is capped at 96 characters because of this: an operator's name carries
  its whole target type, and `operator typename base_type::value_type()` produced a
  419-character path component. Past `NAME_MAX` every filesystem call on that path throws an
  uncaught `filesystem_error` and the splitter aborts instead of falling back.
