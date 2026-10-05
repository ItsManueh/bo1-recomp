"""Finds code accesses to a global variable (lis rX,hi + lwz/stw/addi rY,lo(rX)).

Usage: python tools/xref_global.py <image.bin> <hex_address> [<hex_address> ...]
"""
import struct
import sys

sys.path.insert(0, __file__.replace('\\', '/').rsplit('/', 1)[0])

from find_missing_funcs import sections as _sections

BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
# .text range read from the PE header (works for default.xex and default_mp.xex).
TEXT_LO, TEXT_HI = next((va, va + size) for name, va, size, x in _sections(img) if name == ".text")
OPS = {32: "lwz", 36: "stw", 14: "addi", 34: "lbz", 38: "stb", 48: "lfs", 40: "lhz"}


def w(va):
    return struct.unpack_from(">I", img, va - BASE)[0]


for a in sys.argv[2:]:
    target = int(a, 16)
    hi = ((target + 0x8000) >> 16) & 0xFFFF
    lo = target & 0xFFFF
    refs = []
    for va in range(TEXT_LO, TEXT_HI, 4):
        word = w(va)
        if word >> 26 != 15 or (word >> 16) & 0x1F != 0 or word & 0xFFFF != hi:
            continue
        rx = (word >> 21) & 0x1F
        for d in range(1, 24):
            w2 = w(va + d * 4)
            op = w2 >> 26
            if op in OPS and (w2 >> 16) & 0x1F == rx and w2 & 0xFFFF == lo:
                refs.append(f"{OPS[op]}@{va + d * 4:08X}")
                break
            # The base register is overwritten: stop following it.
            if (w2 >> 21) & 0x1F == rx and op not in (36, 38, 44, 37):
                break
    print(f"{target:08X}: {len(refs)} refs -> {refs}")
