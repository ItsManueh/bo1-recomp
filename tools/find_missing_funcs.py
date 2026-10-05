"""Finds functions the rexglue analysis did not register.

Crosses the functions registered in generated/<module>/bo1_init.cpp with:
  1) code pointers stored in data sections (vtables, callbacks)
  2) the 6-instruction vtable thunk pattern
     lwz r11,X(r3) / addi r3,r11,Y / lwz r11,Y(r11) / lwz r11,Z(r11) / mtctr r11 / bctr

Usage: python tools/find_missing_funcs.py <image.bin> <bo1_init.cpp> [--toml]
With --toml prints entries ready to paste into the [functions] section.
"""
import bisect
import re
import struct
import sys

BASE = 0x82000000

BLR = 0x4E800020
BCTR = 0x4E800420


def sections(image):
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    assert image[e_lfanew:e_lfanew + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", image, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", image, e_lfanew + 20)[0]
    off = e_lfanew + 24 + opt_size
    out = []
    for i in range(nsec):
        s = image[off + i * 40:off + (i + 1) * 40]
        name = s[:8].rstrip(b"\0").decode(errors="replace")
        vsize, va = struct.unpack_from("<II", s, 8)
        chars = struct.unpack_from("<I", s, 36)[0]
        out.append((name, BASE + va, vsize, bool(chars & 0x20000000)))
    return out


def word(image, va):
    return struct.unpack_from(">I", image, va - BASE)[0]


def is_terminator(w):
    # blr, bctr, unconditional branch (no link), or zero padding
    return w in (BLR, BCTR, 0) or (w >> 26 == 18 and w & 3 == 0)


def is_thunk(image, va):
    w = [word(image, va + i * 4) for i in range(6)]
    return (w[0] & 0xFFFF0000 == 0x81630000 and  # lwz r11,X(r3)
            w[1] & 0xFFFF0000 == 0x386B0000 and  # addi r3,r11,Y
            w[2] & 0xFFFF0000 == 0x816B0000 and  # lwz r11,Y(r11)
            w[3] & 0xFFFF0000 == 0x816B0000 and  # lwz r11,Z(r11)
            w[4] == 0x7D6903A6 and w[5] == BCTR)


def main(image_path, init_path, as_toml):
    image = open(image_path, "rb").read()
    registered = sorted({int(m, 16) for m in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), sub_",
                                                         open(init_path).read())})
    reg_set = set(registered)
    secs = sections(image)
    code = [(va, va + size) for _, va, size, x in secs if x]

    def in_code(a):
        return a % 4 == 0 and any(lo <= a < hi for lo, hi in code)

    print("# Sections: " + ", ".join(f"{n}{'(x)' if x else ''}@{va:08X}+{sz:X}"
                                     for n, va, sz, x in secs), file=sys.stderr)
    print(f"# Registered functions: {len(registered)}", file=sys.stderr)

    found = {}

    # 1) Code pointers in data sections
    for name, va, size, x in secs:
        if x:
            continue
        end = min(va + size, BASE + len(image))
        run = []
        for a in range(va, end - 3, 4):
            v = word(image, a)
            if in_code(v):
                run.append((a, v))
                continue
            # End of a run of pointers: if it looks like a vtable (>= 2 pointers, one of them
            # registered) it is used.
            if len(run) >= 2 and any(t in reg_set for _, t in run):
                for src, t in run:
                    if t not in reg_set and is_terminator(word(image, t - 4)):
                        found.setdefault(t, f"vtable in {name}@{src:08X}")
            run = []

    # 2) Thunk pattern across all the code
    for lo, hi in code:
        for a in range(lo, hi - 24, 4):
            if a not in reg_set and is_thunk(image, a) and is_terminator(word(image, a - 4)):
                found.setdefault(a, "vtable thunk")

    print(f"# Unregistered candidates: {len(found)}", file=sys.stderr)
    for a in sorted(found):
        i = bisect.bisect_right(registered, a) - 1
        parent = f"inside sub_{registered[i]:08X}" if i >= 0 else "before the first function"
        if as_toml:
            print(f"0x{a:08X} = {{}}  # {found[a]}; {parent}")
        else:
            print(f"{a:08X}  {found[a]:32}  {parent}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    main(args[0], args[1], "--toml" in sys.argv)
