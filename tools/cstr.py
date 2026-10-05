"""Prints the C strings of the image at the given addresses.

Usage: python tools/cstr.py <image.bin> <hex_address> [<hex_address> ...]
"""
import sys

BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
for a in sys.argv[2:]:
    va = int(a, 16)
    off = va - BASE
    end = img.index(b"\0", off)
    print(f"{va:08X}: {img[off:end].decode(errors='replace')!r}")
