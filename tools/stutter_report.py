"""Crosses the tools/sampler samples (6th argument, .tsv) with the hitches in the game log.

For every slow frame ("bo1: hitch N ms (cause)") summarizes what each thread was doing during that
frame, and at the end compares the hitches with the normal frames.

Usage: python tools/stutter_report.py samples.tsv logs/match.log [threads]
  threads: name parts separated by commas (default "Main,GPU Commands,Backend,Server").
Game functions are shown with the names from config/symbols_tu11.toml (campaign) or
config/symbols_mp_tu11.toml (multiplayer, when the log is of bo1mp), see tools/name_functions.py.
"""

import collections
import datetime
import os
import re
import sys

SYMBOL = re.compile(r'^0x([0-9A-F]{8}) = "([^"]+)"')
GUEST = re.compile(r"sub_([0-9A-F]{8})")
NAMES = {}


def load_symbols(log_path):
    """Names of the game functions for the executable that wrote the log."""
    is_mp = False
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for _, line in zip(range(200), f):
            if "bo1mp" in line or "Multiplayer" in line:
                is_mp = True
                break
    config = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config")
    path = os.path.join(config, "symbols_mp_tu11.toml" if is_mp else "symbols_tu11.toml")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            m = SYMBOL.match(line)
            if m:
                NAMES[m.group(1)] = m.group(2)


def named(text):
    return GUEST.sub(lambda m: f"sub_{m.group(1)}[{NAMES[m.group(1)]}]"
                     if m.group(1) in NAMES else m.group(0), text)

OS_MODULES = ("ntdll!", "KERNELBASE!", "kernel32!", "KERNEL32!", "win32u!", "VCRUNTIME140!",
              "ucrtbase!", "MSVCP140!", "msvcp_win!")
HITCH = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\].*bo1: hitch (\d+) ms(.*)$")


def signature(stack):
    """First frames outside the OS, plus the first game function if it appears further down."""
    frames = [f for f in stack.split(" < ") if f]
    useful = [f for f in frames if not f.startswith(OS_MODULES)]
    head = useful[:3]
    guest = next((f for f in useful if "sub_8" in f), None)
    if guest and guest not in head:
        head.append(guest)
    names = [named(f.split("!", 1)[-1].replace("rex::graphics::", "").replace("rex::", ""))
             for f in head]
    return " < ".join(names) if names else frames[0] if frames else "?"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    wanted = (sys.argv[3] if len(sys.argv) > 3 else "Main,GPU Commands,Backend,Server").split(",")
    load_symbols(sys.argv[2])

    samples = collections.defaultdict(list)  # thread -> [(ms, signature)]
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3:
                continue
            thread = next((w for w in wanted if w in parts[1]), None)
            if thread:
                samples[thread].append((float(parts[0]), signature(parts[2])))
    if not samples:
        print("no samples of the requested threads")
        return 1
    first = min(s[0][0] for s in samples.values())
    last = max(s[-1][0] for s in samples.values())

    hitches = []
    with open(sys.argv[2], encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HITCH.match(line)
            if not m:
                continue
            end = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1000
            start = end - int(m.group(2))
            if start >= first and end <= last:
                hitches.append((start, end, int(m.group(2)), m.group(3).strip()))
    print(f"{len(hitches)} hitches within the {(last - first) / 1000:.0f} s sampled")

    in_hitch = collections.defaultdict(collections.Counter)
    outside = collections.defaultdict(collections.Counter)
    for thread, items in samples.items():
        for t, sig in items:
            hit = any(s <= t <= e for s, e, _, _ in hitches)
            (in_hitch if hit else outside)[thread][sig] += 1

    for start, end, ms, why in hitches:
        print(f"\n== hitch {ms} ms {why}")
        for thread in wanted:
            c = collections.Counter(sig for t, sig in samples.get(thread, []) if start <= t <= end)
            total = sum(c.values())
            if not total:
                continue
            top = ", ".join(f"{100 * n / total:.0f}% {sig}" for sig, n in c.most_common(2))
            print(f"  {thread}: {top}")

    print("\n==== summary: % of samples in hitches versus normal frames ====")
    for thread in wanted:
        a, b = in_hitch[thread], outside[thread]
        ta, tb = sum(a.values()), sum(b.values())
        if not ta:
            continue
        print(f"\n-- {thread} ({ta} samples in hitches, {tb} outside)")
        for sig, n in a.most_common(8):
            print(f"  {100 * n / ta:5.1f}% in hitches | {100 * b[sig] / max(tb, 1):5.1f}% normal | {sig}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
