"""Disassembles PowerPC (big-endian) code from an image extracted with xex_extract.py.

Usage: python tools/ppcdis.py <image.bin> <hex_address> [before] [after]
  before/after: number of instructions to show around the address (default 16/32).
Needs capstone (pip install capstone).
"""
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000


def main(path, addr, before=16, after=32):
    image = open(path, "rb").read()
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    start = addr - before * 4
    for va in range(start, addr + after * 4, 4):
        raw = image[va - BASE:va - BASE + 4]
        word = struct.unpack(">I", raw)[0]
        insn = next(md.disasm(raw, va), None)
        text = f"{insn.mnemonic:8} {insn.op_str}" if insn else "<unknown/VMX128>"
        mark = ">>" if va == addr else "  "
        print(f"{mark} {va:08X}: {word:08X}  {text}")


if __name__ == "__main__":
    a = sys.argv[1:]
    main(a[0], int(a[1], 16), *(int(x) for x in a[2:4]))
