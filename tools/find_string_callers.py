"""Finds which functions receive executable strings as an argument.

For every string containing the given text, finds the lis rX, hi / addi rY, rX, lo pairs that
load its address and the first 'bl' that follows them (up to 12 instructions later). Counts the
targets: the most repeated one across many format strings is usually the print function.

Usage: python tools/find_string_callers.py <image.bin> <text> [<text> ...]
       (the image is loaded at 0x82000000; pass "name\\0" from bash as $'name\\0' to match a whole
       string)
"""

import collections
import struct
import sys

BASE = 0x82000000

img = open(sys.argv[1], "rb").read()
needles = [n.encode() for n in sys.argv[2:]]


def string_addresses():
    out = {}
    for needle in needles:
        start = 0
        while True:
            i = img.find(needle, start)
            if i < 0:
                break
            s = img.rfind(b"\0", 0, i) + 1
            e = img.find(b"\0", i)
            out[BASE + s] = img[s:e]
            start = i + 1
    return out


strings = string_addresses()
print(f"{len(strings)} strings")
targets = collections.Counter()
examples = collections.defaultdict(list)
words = len(img) // 4
for i in range(words - 1):
    ins = struct.unpack_from(">I", img, i * 4)[0]
    if ins >> 26 != 15:  # addis (lis = addis rX, 0, imm)
        continue
    rd = (ins >> 21) & 31
    ra = (ins >> 16) & 31
    if ra != 0:
        continue
    hi = (ins & 0xFFFF) << 16
    for j in range(i + 1, min(i + 8, words)):
        ins2 = struct.unpack_from(">I", img, j * 4)[0]
        if ins2 >> 26 == 14 and ((ins2 >> 16) & 31) == rd:  # addi rY, rX, lo
            lo = ins2 & 0xFFFF
            if lo & 0x8000:
                lo -= 0x10000
            addr = (hi + lo) & 0xFFFFFFFF
            if addr in strings:
                for k in range(j + 1, min(j + 13, words)):
                    ins3 = struct.unpack_from(">I", img, k * 4)[0]
                    if ins3 >> 26 == 18 and (ins3 & 3) == 1:  # bl
                        disp = ins3 & 0x03FFFFFC
                        if disp & 0x02000000:
                            disp -= 0x04000000
                        target = (BASE + k * 4 + disp) & 0xFFFFFFFF
                        targets[target] += 1
                        if len(examples[target]) < 3:
                            examples[target].append((f"0x{BASE + k * 4:08X}",
                                                     strings[addr][:60]))
                        break
            break
for target, count in targets.most_common(12):
    print(f"0x{target:08X}  {count:4d} calls  e.g. {examples[target]}")
