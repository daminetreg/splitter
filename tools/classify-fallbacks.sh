#!/usr/bin/env bash
#
# Classify the fallbacks of a split build by cause.
#
# The build log cannot be read directly for this. It runs at -j32, so the diagnostics of
# thirty-two translation units interleave, and attributing an error to the unit that
# produced it means guessing. What the log *does* record unambiguously is the passthrough
# command printed next to every fallback -- the exact compiler invocation, with all its
# flags. This replays each of those through the splitter on its own, one at a time, and
# groups the results by their first error.
#
# A fallback that leaves no `error:` line at all failed at the relocatable link instead;
# those are reported separately, by their linker diagnostic.
#
# Usage: ./classify-fallbacks.sh [build.log] [workdir]
#        ./test-boost-libraries.sh && ./classify-fallbacks.sh
set -uo pipefail

LOG="${1:-/tmp/boost-libs-split.log}"
WORK="${2:-/tmp/cpp-splitter-fallbacks}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPLITTER="$REPO/build/cpp-splitter"

[ -r "$LOG" ] || { echo "no such log: $LOG" >&2; exit 1; }
[ -x "$SPLITTER" ] || { echo "build cpp-splitter first" >&2; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK/logs"
grep -A1 'falling back' "$LOG" | grep 'passthrough:' \
    | sed 's/^\[cpp-splitter\] passthrough: //' | sort -u > "$WORK/commands"

total=$(wc -l < "$WORK/commands")
echo "==> $total fallback(s) to replay"
[ "$total" -gt 0 ] || exit 0

# The commands use paths relative to the build directory, so replay them from there.
BUILD_DIR=$(dirname "$LOG")
case "$LOG" in
    /tmp/boost-libs-split.log) BUILD_DIR=/tmp/boost-libs-split ;;
esac
cd "$BUILD_DIR" || exit 1

n=0
while IFS= read -r cmd; do
    n=$((n + 1))
    src=$(sed 's/.* -c //;s/ *$//' <<<"$cmd")
    obj=$(sed -n 's/.* -o \([^ ]*\) .*/\1/p' <<<"$cmd")
    # Without this the split directory is reused and its cache reports success.
    [ -n "$obj" ] && rm -rf "${obj}.split"
    printf '\r    %d/%d %-50.50s' "$n" "$total" "$(basename "$src")" >&2
    $SPLITTER $cmd > "$WORK/logs/$(printf '%03d' "$n")_$(basename "$src" .cpp).log" 2>&1
done < "$WORK/commands"
printf '\r%-60s\r' '' >&2

python3 - "$WORK" <<'PY'
import collections, glob, os, re, sys

work = sys.argv[1]
groups = collections.defaultdict(list)

for path in sorted(glob.glob(os.path.join(work, "logs", "*.log"))):
    text = open(path, errors="replace").read()
    name = os.path.basename(path)
    if "falling back" not in text:
        groups[("no longer falls back", "")].append(name)
        continue
    # `  $ <compiler> ...` lines echo the piece commands; they are not diagnostics.
    errors = [l for l in text.splitlines()
              if " error: " in l and not l.startswith("  $ ")]
    if not errors:
        if "multiple definition" in text:
            groups[("relocatable link: multiple definition", "")].append(name)
        elif "undefined reference" in text:
            groups[("relocatable link: undefined reference", "")].append(name)
        else:
            groups[("failed with no diagnostic", "")].append(name)
        continue
    where, message = errors[0].split(" error: ", 1)
    message = re.sub(r"'[^']*'", "'X'", message)
    where = re.sub(r"^.*\.split/", "<split>/", where.split(":")[0])
    groups[(message, where)].append(name)

for (message, where), members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    print(f"\n{len(members):4d}  {message}")
    if where:
        print(f"      in {where}")
    for m in members[:6]:
        print(f"        {m}")
    if len(members) > 6:
        print(f"        ... and {len(members) - 6} more")
print()
PY
echo "==> per-unit logs in $WORK/logs"
