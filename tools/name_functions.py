"""Names recompiled game functions from evidence in the executable (first decompilation step).

Every translated function is called sub_XXXXXXXX after its address. This script reads the generated
code (which keeps the original PowerPC instructions as comments) and the loaded image, follows the
lis/addi pairs that build addresses, and names functions from:
  1. error and usage messages that start with a function name, e.g. "VEH_GetSeat(): ..." or
     "USAGE: AnimateUI( ..." (the function that references the message);
  2. tables of { "name", function } pairs in the image data (script builtins, console commands,
     menu handlers...): the function is named after the string;
  3. calls that pass a string and a function together (r3 = "name", r4 = function), like
     Cmd_AddCommand("name", handler).

Usage: python tools/name_functions.py <generated_dir> <image.bin> <out.toml>
  e.g. python tools/name_functions.py generated/default assets/default_tu11.image.bin
       config/symbols_tu11.toml
The output maps addresses to names, with the evidence, for the developer tools and for reading the
translated code; nothing in the build depends on it.
"""
import collections
import glob
import os
import re
import struct
import sys

BASE = 0x82000000
FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\)')
LIS = re.compile(r'^\s*// lis r(\d+),(-?\d+)')
ADDI = re.compile(r'^\s*// addi r(\d+),r(\d+),(-?\d+)')
ORI = re.compile(r'^\s*// ori r(\d+),r(\d+),(\d+)')
MR = re.compile(r'^\s*// mr r(\d+),r(\d+)')
BL = re.compile(r'^\s*// bl 0x([0-9a-f]+)')
LABEL_OR_BRANCH = re.compile(r'^\s*// (b|bc|beq|bne|blt|bgt|ble|bge|bctr|blr)\b')
IDENT = re.compile(rb'^[A-Za-z_][A-Za-z0-9_]{2,63}$')
# "Name(): ..." or "Prefix_Name( ..." (an engine function name, not a word like "Parameter (").
NAME_PREFIX = re.compile(r'^([A-Za-z_][A-Za-z0-9_]{2,63})\(\)|^([A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]+) ?\(')
USAGE_PREFIX = re.compile(r'^USAGE: *([A-Za-z_][A-Za-z0-9_]{2,63}) ?\(')


# Functions identified by hand (src/bo1_engine.cpp and the profiling sessions), per image.
KNOWN = {
    'default_tu11': {
        0x82315590: 'Com_Frame',
        0x8230FD58: 'Cbuf_AddText',
        0x82313280: 'Com_Error',
        0x82379648: 'Dvar_FindVar',
        0x8237B9A0: 'Dvar_SetBoolByName',
        0x823109C0: 'Cmd_AddCommand',
        0x824BEED8: 'XInputGetState',
        0x824BEFB0: 'XInputGetKeystroke',
        0x824BDF80: 'Sys_Yield',
        0x8242EF88: 'FindNameInTable',
        0x823857B8: 'String_Compare',
    },
    'default_mp_tu11': {
        0x82343D60: 'Com_Frame',
        0x8233E8D8: 'Cbuf_AddText',
        0x82341CA8: 'Com_Error',
        0x823E2768: 'Dvar_FindVar',
        0x82136C80: 'D3DQuery_GetData',
    },
}


def read_string(image, va, limit=200):
    off = va - BASE
    if off < 0 or off >= len(image):
        return None
    end = image.find(b'\0', off, off + limit)
    if end <= off:
        return None
    raw = image[off:end]
    if not all(32 <= c < 127 for c in raw):
        return None
    return raw


def main():
    gen_dir, image_path, out_path = sys.argv[1:4]
    image = open(image_path, 'rb').read()

    functions = set()
    # Per function: addresses built with lis/addi (strings or code), and (r3 string, r4 function)
    # pairs at calls.
    built = collections.defaultdict(set)
    call_pairs = []
    for path in sorted(glob.glob(os.path.join(gen_dir, '*.cpp'))):
        current = None
        regs = {}
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                current = int(m.group(2), 16)
                functions.add(current)
                regs = {}
                continue
            if current is None:
                continue
            m = LIS.match(line)
            if m:
                regs[int(m.group(1))] = ('hi', (int(m.group(2)) << 16) & 0xFFFFFFFF)
                continue
            m = ADDI.match(line)
            if m:
                dst, src, imm = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if regs.get(src, (None,))[0] == 'hi':
                    value = (regs[src][1] + imm) & 0xFFFFFFFF
                    regs[dst] = ('addr', value)
                    built[current].add(value)
                else:
                    regs.pop(dst, None)
                continue
            m = ORI.match(line)
            if m:
                dst, src, imm = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if regs.get(src, (None,))[0] == 'hi':
                    value = regs[src][1] | imm
                    regs[dst] = ('addr', value)
                    built[current].add(value)
                else:
                    regs.pop(dst, None)
                continue
            m = MR.match(line)
            if m:
                dst, src = int(m.group(1)), int(m.group(2))
                if src in regs:
                    regs[dst] = regs[src]
                else:
                    regs.pop(dst, None)
                continue
            m = BL.match(line)
            if m:
                r3, r4 = regs.get(3), regs.get(4)
                if r3 and r4 and r3[0] == 'addr' and r4[0] == 'addr':
                    call_pairs.append((current, int(m.group(1), 16), r3[1], r4[1]))
                # Volatile registers do not survive a call.
                for r in list(regs):
                    if r <= 12:
                        regs.pop(r)
                continue
            if LABEL_OR_BRANCH.match(line) or line.lstrip().startswith('loc_'):
                # Control flow: forget the tracked values (conservative).
                regs = {}

    names = {}  # address -> (name, evidence)

    def propose(address, name, evidence, priority):
        old = names.get(address)
        if old is None or priority > old[2]:
            names[address] = (name, evidence, priority)

    # 1. Messages that start with a function name.
    message_names = collections.defaultdict(set)
    for func, addresses in built.items():
        for va in addresses:
            raw = read_string(image, va)
            if not raw:
                continue
            text = raw.decode('ascii')
            m = NAME_PREFIX.match(text)
            if m:
                message_names[m.group(1) or m.group(2)].add(func)
                propose(func, m.group(1) or m.group(2), f'message "{text[:60]}"', 3)
                continue
            m = USAGE_PREFIX.match(text)
            if m:
                propose(func, 'Script_' + m.group(1), f'message "{text[:60]}"', 2)

    # A message used by several functions: the check (and its message) was inlined into callers,
    # so none of them is surely the named function.
    for name, funcs in message_names.items():
        if len(funcs) > 1:
            for func in funcs:
                if names.get(func, (None,))[0] == name:
                    names[func] = ('uses_' + name, names[func][1] + ' (inlined check)', 2)

    # 2. { "name", function } tables in the data (big-endian pointers).
    for off in range(0, len(image) - 8, 4):
        name_ptr, func_ptr = struct.unpack_from('>II', image, off)
        if func_ptr not in functions:
            continue
        raw = read_string(image, name_ptr, 64)
        if raw and IDENT.match(raw):
            propose(func_ptr, 'Table_' + raw.decode('ascii'), f'table at {BASE + off:08X}', 1)

    # 3. Calls passing a name and a function.
    by_callee = collections.Counter(callee for _, callee, _, _ in call_pairs)
    for caller, callee, s_ptr, f_ptr in call_pairs:
        if f_ptr not in functions:
            continue
        raw = read_string(image, s_ptr, 64)
        if raw and IDENT.match(raw):
            propose(f_ptr, 'Cmd_' + raw.decode('ascii'),
                    f'call {callee:08X}("{raw.decode()}", fn) in sub_{caller:08X}', 1)

    # Hand-identified functions win over everything.
    image_key = os.path.basename(image_path).split('.')[0]
    for address, name in KNOWN.get(image_key, {}).items():
        propose(address, name, 'identified by hand', 10)

    # The callees used most for (name, function) registrations are registration functions.
    for callee, count in by_callee.most_common(5):
        if count >= 20:
            propose(callee, f'Register_{callee:08X}', f'{count} name+function calls', 0)

    with open(out_path, 'w', encoding='utf-8', newline='\n') as out:
        out.write('# Function names identified by tools/name_functions.py (do not edit by hand).\n')
        out.write(f'# {len(names)} of {len(functions)} translated functions named.\n')
        out.write('# address = "name"  # evidence\n\n[names]\n')
        for address in sorted(names):
            name, evidence, _ = names[address]
            evidence = evidence.replace('"', "'")
            out.write(f'0x{address:08X} = "{name}"  # {evidence}\n')
    print(f'{len(names)} of {len(functions)} functions named -> {out_path}')


if __name__ == '__main__':
    main()
