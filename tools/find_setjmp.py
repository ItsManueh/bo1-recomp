"""Finds setjmp/longjmp in an Xbox 360 image.

setjmp stores the non-volatile registers (r13..r31, f14..f31) in a block into the r3 buffer;
longjmp restores them from r3 and jumps to the saved address (mtlr/blr). This looks for runs of
std/stw (save) or ld/lwz (restore) with base r3 and registers >= r13.

Usage: python tools/find_setjmp.py <image.bin>
"""
import struct
import sys

sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
from find_missing_funcs import BASE, sections  # noqa: E402

img = open(sys.argv[1], "rb").read()
lo, hi = next((va, va + s) for n, va, s, x in sections(img) if n == ".text")


def w(va):
    return struct.unpack_from(">I", img, va - BASE)[0]


def kind(word):
    op = word >> 26
    rs, ra = (word >> 21) & 31, (word >> 16) & 31
    if ra != 3 or rs < 13:
        return None
    if op == 62 and word & 3 == 0:
        return "save"     # std
    if op == 36:
        return "save"     # stw
    if op == 58 and word & 3 == 0:
        return "load"     # ld
    if op == 32:
        return "load"     # lwz
    if op == 54:
        return "fsave"    # stfd
    if op == 50:
        return "fload"    # lfd
    return None


results = []
a = lo
while a < hi:
    k = kind(w(a))
    if k in ("save", "load"):
        start, regs = a, set()
        while a < hi and kind(w(a)) in (k, "f" + k):
            if kind(w(a)) == k:
                regs.add((w(a) >> 21) & 31)
            a += 4
        if len(regs) >= 15:
            # Walk back to the start of the function (after the previous blr/b).
            f = start
            while f > lo and w(f - 4) not in (0x4E800020, 0) and (w(f - 4) >> 26) != 18 and start - f < 64:
                f -= 4
            results.append((k, f, start, len(regs)))
    else:
        a += 4

for k, f, start, n in results:
    name = "setjmp" if k == "save" else "longjmp"
    print(f"{name:8} candidate: function ~0x{f:08X}, run at 0x{start:08X}, {n} registers")
