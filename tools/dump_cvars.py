"""Lists the cvars (configuration options) compiled into the ReXGlue runtime binaries.

Cvars are registered with (name, default value, category, description); this looks for
identifier-like strings followed by a known category and a description.
Usage: python tools/dump_cvars.py <dll_or_exe> [...]
"""
import re
import sys

ident = re.compile(rb"^[a-z][a-z0-9_]{2,63}$")

for path in sys.argv[1:]:
    data = open(path, "rb").read()
    strings = [m.group() for m in re.finditer(rb"[\x20-\x7e]{3,}", data)]
    cats = {s for s in strings if re.fullmatch(rb"[A-Z][A-Za-z0-9 /]{1,24}", s)}
    print(f"===== {path}")
    seen = set()
    for i, s in enumerate(strings[:-2]):
        if not ident.match(s) or s in seen:
            continue
        window = strings[i + 1:i + 4]
        cat = next((w for w in window if w in cats), None)
        desc = next((w for w in window if len(w) > 20 and b" " in w), None)
        if cat and desc:
            seen.add(s)
            print(f"{s.decode():40} [{cat.decode()}] {desc.decode()[:110]}")
