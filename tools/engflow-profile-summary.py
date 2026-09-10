#!/usr/bin/env python3
"""Summarise an EngFlow invocation profile: where a distributed build's time went.

CMake RE prints `Invocation ID: <uuid>` for every `--distributed` build. The cluster keeps a
Chrome-tracing profile of that invocation, which is a far better answer to "why was this row
slow" than a wall clock:

    curl --cert ~/engflow-mTLS/engflow.crt --key ~/engflow-mTLS/engflow.key \
         https://$RBE_service/api/profiling/v1/instances/default/invocations/$ID -o prof.json
    tools/engflow-profile-summary.py prof.json

The file is hundreds of megabytes but holds one JSON object per line, so it is read as a stream
rather than parsed whole -- json.load() on it needs several gigabytes.

Durations are summed across all workers, so they add up to far more than the wall clock: that
is the point, since it is what parallelism is spending. The wall clock is reported separately
as the span of the trace.
"""
import collections
import json
import sys


def events(path):
    with open(path) as f:
        for line in f:
            line = line.strip().rstrip(",")
            if not line.startswith("{"):
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def main(argv):
    if len(argv) != 2:
        sys.exit(f"usage: {argv[0]} <profile.json>")

    by_name = collections.defaultdict(lambda: [0, 0])      # name -> [count, total us]
    by_mnemonic = collections.defaultdict(lambda: [0, 0])
    lo, hi = None, None

    for e in events(argv[1]):
        if e.get("ph") != "X":
            continue
        name, dur, ts = e.get("name", "?"), e.get("dur", 0), e.get("ts")
        by_name[name][0] += 1
        by_name[name][1] += dur
        mnemonic = (e.get("args") or {}).get("action_mnemonic")
        if mnemonic:
            by_mnemonic[mnemonic][0] += 1
            by_mnemonic[mnemonic][1] += dur
        if ts is not None:
            lo = ts if lo is None else min(lo, ts)
            hi = ts + dur if hi is None else max(hi, ts + dur)

    if lo is None:
        sys.exit("no timed events in this profile")

    print(f"trace span      {(hi - lo) / 1e6:8.1f}s")
    print(f"summed duration {sum(v[1] for v in by_name.values()) / 1e6:8.1f}s"
          f"   (across all workers, so larger than the span)")

    def table(title, data):
        print(f"\n{title:<28} {'count':>8} {'total':>10} {'mean':>9}")
        print("-" * 58)
        for name, (n, total) in sorted(data.items(), key=lambda kv: -kv[1][1])[:15]:
            print(f"{name[:28]:<28} {n:>8} {total/1e6:>9.1f}s {total/n/1000:>8.1f}ms")

    table("event", by_name)
    if by_mnemonic:
        table("action mnemonic", by_mnemonic)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
