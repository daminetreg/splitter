# 49 — moving virtual member functions instead of keeping them

## Motivation

Every virtual member function is kept today, whatever it looks like: `prepare_functions()`
puts `fn.is_virtual` beside templates and explicit specializations in the keep rule, and
`keep_reason()` says "virtual member function". The reason recorded in `visitor()` is
narrow — `override` and `final` are legal only on the in-class declaration, they are often
spelled as macros (`BOOST_OVERRIDE`, `CV_OVERRIDE`), and stripping them from a rebuilt
out-of-line declarator by text matching cannot find a macro. So the rule covers more than
the problem: an out-of-line virtual in a `.cpp` (`void Widget::draw() { … }`) has no
declarator to rebuild and no specifier to strip, and is kept all the same.

What a kept virtual costs is the usual cost of a keep: in a header, a copy in every piece
(harmless, it is `inline`) and a recompile of every includer when its body is edited; in a
`.cpp`, the body sits in the preamble and every piece of that unit re-parses it, and an edit
to it re-splits the unit rather than re-slicing one piece. Virtuals are where the bodies are
in a lot of C++ — visitors, listeners, `dbprint()` in p4c, every `Node::accept()` — so the
count is not small. It has not been measured; the first step below measures it.

What makes a virtual different from any other member is not the keyword but two things the
compiler decides from it:

- **The vtable follows the key function.** A class's vtable and typeinfo are emitted in the
  translation unit that defines its key function, the first non-inline, non-pure virtual
  member declared in the class. Every other translation unit expects the vtable from there.
  A definition that was not `inline` must therefore stay not-`inline` wherever it moves:
  TODO/47's P4Lexer failure was exactly a virtual key function made `inline` by the split
  (`undefined symbol: vtable for P4::P4Lexer`). Inside one unit the pieces are joined by
  `ld -r` into one object, so *which piece* defines the key function does not matter to the
  rest of the program; only its linkage does.
- **`override` and `final` are declaration-only.** The rebuilt out-of-line form must drop
  them, and the in-class declaration left behind must keep them. Spelled as keywords they
  are already handled (`strip_virt_specifiers()`); spelled as macros they are not, and text
  cannot tell `BOOST_OVERRIDE` from an identifier.

Neither is an obstacle for the whole set. They divide it.

## Implementation Proposal

### Step 0 — measure

`grep -h 'virtual member function' */*.keeps` across the split trees of the benchmarks —
Boost.Filesystem, Boost.Spirit's suite, Boost.Geometry, OpenCV, p4c — and count per
category: out-of-line in a `.cpp`, out-of-line in a header (`.ipp`), in-class in a header.
The rows of `benchmarks/` say what an edit to one of them costs today. This decides whether
Step B is worth its complexity; Step A is cheap regardless.

### Step A — out-of-line virtuals in a source file

A definition written `T C::f(args) { … }` at namespace scope in the unit being compiled.
Nothing to rebuild: the class (in a header, or in the preamble) declares it with its
`virtual`, `override` and `final`; the definition text moves to a piece as any out-of-line
member does. `inline` is stripped from `.cpp` pieces already, and a virtual written out of
line was not inline, so the key-function rule is untouched: the piece defines it with
external linkage, the vtable is emitted in that piece, `ld -r` puts it in the unit's object.

- `prepare_functions()`: the `is_virtual` keep applies only when `fn.defined_in_class ||
  is_included_file(fn.file)`. A virtual destructor out of line is covered by the same
  change (the ctor/dtor keep is for headers only already).
- Nothing in `generate_preamble()` changes: the definition range becomes nothing, the
  class already declares the member.
- Pure virtuals with a body (`void f() = 0;` plus `void C::f() {}`) move the same way.

### Step B — in-class virtuals in headers

A definition written inside the class body, so implicitly `inline`, in a header the unit
splits. Moving it means the class keeps `virtual T f(args) override;` and the piece gets
`inline T C::f(args) { … }` with `__attribute__((used))`, as every header piece does. It
was `inline`, it stays `inline`, so the class has no key function before and after: the
vtable is weak and emitted where used, in both builds. The piece's `used` copy of `f` is
what those weak vtables point at, exactly as for a non-virtual inline member today.

- **Specifiers from the AST, not the text.** libclang exposes `override` and `final` as
  attribute cursors under the method — `CXCursor_CXXOverrideAttr`, `CXCursor_CXXFinalAttr`
  — each with an extent; when the keyword came from a macro, the extent is the macro
  invocation in the source (`clang_getRangeStart` of the spelling location). Record those
  ranges on `FunctionInfo` in `visitor()` and blank them when rebuilding the out-of-line
  declarator, instead of matching the words. `strip_virt_specifiers()` stays for the
  spelled form and becomes a check that nothing is left.
- **Covariant returns** already go through the trailing-return rebuild (`auto C::f() ->
  T`), which is what makes a class-scoped return type resolvable out of line.
- **Still kept**: virtuals in class templates (kept as members of templates), a `final`
  class's members? — no, `final` on the class is not on the declarator; they move. Virtual
  destructors in headers stay under the ctor/dtor rule (TODO's C1/C2, D0/D1/D2 family).
  A virtual that is also the key function cannot be in-class (it would be inline), so the
  key-function rule never applies here.
- `collect_emitted()`'s skip of `is_virtual` in the emission set goes with the keep rule:
  a header virtual moves only when this unit emits it, like any other header definition.

### Step C — out-of-line virtuals in headers (`.ipp`)

`inline void C::f() { … }` written out of line in an implementation include. No declarator
rebuild (it is out of line), `inline` present and kept, `used` added: the header-piece path
as it is. Only the keep rule stands in the way; it goes with Step A's condition. A
non-`inline` out-of-line virtual in a header is a key function defined in a header — legal
only if exactly one unit includes it — and moves as in Step A, `inline` not added.

## Tests

`split.virtual_members` (`test/virtual_members_main.cpp`, `test/virtual_members.hpp`,
`test/virtual_override_macro.hpp`): a base with a pure virtual and a virtual with a body, a
derived class in a header with in-class `override` spelled as a keyword and as a macro
`VM_OVERRIDE`, a `final` override, a covariant return, and a derived class in the source
with its overrides out of line; the program calls through base pointers and returns 0 only
on the expected values. After each step:

- A: pieces exist for the source's out-of-line virtuals; `.keeps` no longer lists them;
  `nm` of the split object shows the vtable of the source's class defined (`V`/`T`, not
  `U`), and the program runs.
- B: pieces exist for the header's in-class virtuals; the rewritten header declares them
  with `override`/`VM_OVERRIDE` intact; the pieces carry neither; program runs.
- Byte-identity of the re-slice against a full split, as `launcher.incremental_body_edit`
  checks, after an edit to a moved virtual body.

`launcher.virtual_key_function`: the split unit defines the key function of a class; a
second unit compiled *plain* constructs that class; the two link (the vtable is in the
split object) and run. This is the P4Lexer shape and fails today on any path that adds
`inline`.

Then the example builds in the order CLAUDE.md gives: `example/boost-to-split` Filesystem
with 0 fallbacks, then the Spirit suite through
`example/spirit-tests/SpiritTestsFromJamfiles.cmake`, both with the same results as before;
and p4c, which has the most virtuals of anything measured here, at TODO/47's numbers or
better.

## Acceptance Criteria

- Step 0's count is in this file, per category and per project.
- `split.virtual_members` and `launcher.virtual_key_function` pass; each fails before its
  step.
- No keep reads "virtual member function" for an out-of-line virtual in a `.cpp` (Step A)
  or for an in-class virtual with a spelled or macro `override` in a header this unit
  emits (Step B); the remaining keeps are virtual destructors in headers and members of
  templates, with those reasons.
- Boost.Filesystem, Boost.Spirit and OpenCV: 0 fallbacks, programs agree with plain; p4c's
  one-body row does not regress.
- The `benchmarks/` one-body row for p4c on a virtual body: one piece recompiled where the
  whole unit was re-split before.
