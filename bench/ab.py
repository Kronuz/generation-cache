#!/usr/bin/env python3
"""Alternate several generation_cache_bench binaries over rounds and print, per cell, the fastest
and the spread (max/min) of each binary. A binary that appears twice under different names is an A/A.

usage: bench_ab.py ROUNDS name=path [name=path ...] -- bench-args...
Rows parsed:  n=<n> <comparator> 1 thread <x> ns   4 threads <y> ns/op
              <comparator> construct+destroy <a> ns   get hit <b> ns   get miss <c> ns
"""
import re
import subprocess
import sys
from collections import defaultdict

ROW = re.compile(r"n=(\d+)\s+(.+?)\s+1 thread\s+([\d.]+) ns\s+4 threads\s+([\d.]+) ns/op")
SMALL = re.compile(r"^\s+(.+?)\s+construct\+destroy\s+([\d.]+) ns\s+get hit\s+([\d.]+) ns\s+get miss\s+([\d.]+) ns")


def main():
    rounds = int(sys.argv[1])
    sep = sys.argv.index("--")
    bins = [a.split("=", 1) for a in sys.argv[2:sep]]
    args = sys.argv[sep + 1:]
    cells = defaultdict(lambda: defaultdict(list))  # (row label) -> binary name -> samples
    order = []
    for r in range(rounds):
        for name, path in bins:
            out = subprocess.run([path] + args, capture_output=True, text=True, check=True).stdout
            for line in out.splitlines():
                m = ROW.search(line)
                if m:
                    n, comp, t1, t4 = m.groups()
                    for label, v in ((f"{n} keys, 1 thread | {comp}", t1), (f"{n} keys, 4 threads | {comp}", t4)):
                        if label not in cells: order.append(label)
                        cells[label][name].append(float(v))
                    continue
                m = SMALL.search(line)
                if m:
                    comp, c, h, mi = m.groups()
                    for label, v in ((f"64 int: construct+destroy | {comp}", c), (f"64 int: get hit | {comp}", h), (f"64 int: get miss | {comp}", mi)):
                        if label not in cells: order.append(label)
                        cells[label][name].append(float(v))
        print(f"round {r + 1}/{rounds} done", file=sys.stderr)
    names = [n for n, _ in bins]
    print(f"{'row':58s} " + " ".join(f"{n:>16s}" for n in names))
    for label in order:
        row = cells[label]
        print(f"{label:58s} " + " ".join(f"{min(row[n]):8.2f} ({max(row[n]) / min(row[n]):4.2f}x)" for n in names))


if __name__ == "__main__":
    main()
