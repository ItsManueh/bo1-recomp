"""Classifies the find_missing_funcs.py candidates and generates safe [functions] entries.

Criteria (the same ones applied by hand to default.xex):
  - DATA:   the first instructions cannot be decoded -> dropped.
  - SEH:    __except/__finally block (uses r31 as frame, 'mr r8,r8', 'addi r12/r1,r31', returns an
            HRESULT with lis r3,0x8xxx or is a filter comparing against 0xC000xxxx)
            -> dropped: it is NOT a function.
  - THUNK:  'addi r3,r3,N ; b X' (8 bytes), 'b X' (4 bytes) or the 6-instruction vtable thunk.
  - FUNC:   the rest, only if no branch in the code jumps there (otherwise it is an inner label).

Usage: python tools/classify_candidates.py <image.bin> <candidates.txt>
Prints TOML to paste into [functions] and, as comments, the dropped ones.
"""
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from find_missing_funcs import BASE, is_thunk, sections  # noqa: E402

img = open(sys.argv[1], "rb").read()
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
code = [(va, va + size) for _, va, size, x in sections(img) if x]


def word(va):
    return struct.unpack_from(">I", img, va - BASE)[0]


def dis(va):
    raw = img[va - BASE:va - BASE + 4]
    ins = next(md.disasm(raw, va), None)
    return (ins.mnemonic, ins.op_str) if ins else None


def branch_targets():
    hits = set()
    for lo, hi in code:
        for a in range(lo, hi, 4):
            w = word(a)
            op = w >> 26
            if op == 18:
                off = w & 0x03FFFFFC
                if off & 0x02000000:
                    off -= 0x04000000
            elif op == 16:
                off = w & 0xFFFC
                if off & 0x8000:
                    off -= 0x10000
            else:
                continue
            hits.add((off if w & 2 else a + off) & 0xFFFFFFFF)
    return hits


def classify(a, targets):
    ins = [dis(a + i * 4) for i in range(4)]
    if ins[0] is not None and ins[0][0] == "b":
        return "THUNK", 4  # may be followed by padding (0x00000000)
    if ins[0] is None or ins[1] is None:
        return "DATA", None
    text = " ; ".join(f"{i[0]} {i[1]}" for i in ins if i)
    if is_thunk(img, a):
        return "THUNK", 24
    # SEH filter: compares the exception code (r3) with an NTSTATUS constant 0xC000xxxx.
    if any(i and i[0] == "lis" and i[1].endswith(", -0x4000") for i in ins):
        return "SEH", None
    if ins[0] == ("addi", ins[0][1]) and ins[0][1].startswith("r3, r3,") and ins[1][0] == "b":
        return "THUNK", 8
    seh_markers = ("(r31)", "r8, r8", "r12, r31", "r1, r31")
    if any(m in text for m in seh_markers) or (ins[0][0] == "lis" and ins[0][1].startswith("r3, -0x")):
        return "SEH", None
    if a in targets:
        return "LABEL", None
    return "FUNC", 0


targets = branch_targets()
accepted, rejected = [], []
for line in open(sys.argv[2], encoding="utf-8-sig"):
    if not line.strip() or line.startswith("#"):
        continue
    a = int(line.split()[0], 16)
    kind, size = classify(a, targets)
    first = dis(a)
    desc = f"{first[0]} {first[1]}" if first else "?"
    if kind in ("THUNK", "FUNC"):
        body = f"{{ size = {size} }}" if size else "{}"
        accepted.append(f"0x{a:08X} = {body:16} # {kind.lower()}: {desc}")
    else:
        rejected.append(f"# dropped 0x{a:08X} ({kind}): {desc}")

print("\n".join(accepted))
print("\n".join(rejected))
print(f"# accepted: {len(accepted)}, dropped: {len(rejected)}", file=sys.stderr)
