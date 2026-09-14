#!/usr/bin/env python3
"""Function-by-function comparison of two ELF files' code.

Disassembles both with objdump, cuts the output into functions, strips what layout alone
decides -- the addresses, the numeric form of jump and call targets, the displacement of
rip-relative operands, immediates wide enough to be addresses in a non-PIE executable, the
int3/nop padding after a function -- and reports which functions exist in one file only,
which have the same instructions in both, and which differ, with instruction counts. PLT
stubs and the C runtime's start-up code are left out: they differ by layout only. The full
diff of every differing function goes to the file named by --diff.

    asmdiff.py A B [--diff out.diff] [--objdump objdump]
"""
import argparse
import difflib
import re
import subprocess

HEADER = re.compile(r"^[0-9a-f]+ <(.+)>:$")
ADDR = re.compile(r"^\s*[0-9a-f]+:\s*")
TARGET = re.compile(r"\b[0-9a-f]+ <([^>]+)>")       # `401150 <foo+0x20>` -> `<foo+0x20>`
RIP = re.compile(r"-?0x[0-9a-f]+\(%rip\)")            # `0x2f0d(%rip)` -> `DISP(%rip)`
IMM_ADDR = re.compile(r"\$0x[0-9a-f]{6,}")              # `$0x2009df` (a .rodata address) -> `$ADDR`
COMMENT = re.compile(r"\s+#.*$")
PADDING = {"int3", "nop", "nopl", "nopw", "xchg   %ax,%ax", "cs nopw"}
RUNTIME = {"_start", "_init", "_fini", "deregister_tm_clones", "register_tm_clones",
           "__do_global_dtors_aux", "frame_dummy", ".plt", ".plt.got", ".plt.sec"}


def is_runtime(name):
    return name in RUNTIME or "@plt" in name


def functions(path, objdump):
    text = subprocess.run([objdump, "-d", "--no-show-raw-insn", "-C", path],
                          check=True, capture_output=True, text=True).stdout
    out, name, body = {}, None, []
    for line in text.splitlines():
        m = HEADER.match(line)
        if m:
            if name is not None:
                out[name] = body
            name, body = m.group(1), []
            continue
        if name is None or not ADDR.match(line):
            continue
        insn = ADDR.sub("", line)
        insn = COMMENT.sub("", insn)
        insn = TARGET.sub(r"<\1>", insn)
        insn = RIP.sub("DISP(%rip)", insn)
        insn = IMM_ADDR.sub("$ADDR", insn)
        body.append(insn.strip())
    if name is not None:
        out[name] = body
    for name, body in out.items():
        while body and body[-1].split()[0] in PADDING:
            body.pop()
    return {n: b for n, b in out.items() if not is_runtime(n)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--diff")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    args = ap.parse_args()

    fa, fb = functions(args.a, args.objdump), functions(args.b, args.objdump)
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    common = sorted(set(fa) & set(fb))
    same = [n for n in common if fa[n] == fb[n]]
    differ = [n for n in common if fa[n] != fb[n]]

    print(f"functions: {len(fa)} in {args.label_a}, {len(fb)} in {args.label_b}; "
          f"{len(same)} identical, {len(differ)} differ, "
          f"{len(only_a)} only in {args.label_a}, {len(only_b)} only in {args.label_b}")
    insn_a = sum(len(v) for v in fa.values())
    insn_b = sum(len(v) for v in fb.values())
    print(f"instructions: {insn_a} in {args.label_a}, {insn_b} in {args.label_b}")
    for n in differ:
        print(f"  differ   {n}  ({len(fa[n])} -> {len(fb[n])} instructions)")
    for n in only_a:
        print(f"  only {args.label_a}   {n}  ({len(fa[n])} instructions)")
    for n in only_b:
        print(f"  only {args.label_b}   {n}  ({len(fb[n])} instructions)")

    if args.diff:
        with open(args.diff, "w") as f:
            for n in differ:
                f.write("".join(difflib.unified_diff(
                    [l + "\n" for l in fa[n]], [l + "\n" for l in fb[n]],
                    fromfile=f"{args.label_a}: {n}", tofile=f"{args.label_b}: {n}", n=2)))
                f.write("\n")
            for label, names, fns in ((args.label_a, only_a, fa), (args.label_b, only_b, fb)):
                for n in names:
                    f.write(f"=== only in {label}: {n}\n")
                    f.write("".join(l + "\n" for l in fns[n]))
                    f.write("\n")


if __name__ == "__main__":
    main()
