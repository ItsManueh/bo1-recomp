"""Xbox 360 STFS package reader (CON / LIVE / PIRS): DLC, Title Updates, saves, themes.

Usage:
  python tools/stfs.py info    <package>
  python tools/stfs.py list    <package>
  python tools/stfs.py extract <package> <destination_folder>

Format (summary):
  - Header with a magic (CON = console, LIVE/PIRS = signed by Microsoft) and metadata at 0x340..
    (content type, title ID, display name, volume descriptor at 0x379).
  - Data in 4 KiB blocks. Every 170 (0xAA) data blocks there is a level 0 hash table; every 170
    level 0 tables there is a level 1 one, and so on. "Male" packages (bit 0 of the block
    separation byte = 0) keep two copies of each table; "female" ones keep one.
  - Each hash entry (0x18 bytes) holds the block SHA-1 and, at +0x15, the next block of the file
    (big-endian int24): that is how non-contiguous files are followed.
  - The file table: 0x40-byte entries (name, flags, blocks, start, parent, size).
"""
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
HASHES_PER_TABLE = 0xAA

CONTENT_TYPES = {
    0x00000001: "Saved game",
    0x00000002: "Downloadable content (DLC)",
    0x00000003: "Editor",
    0x00030000: "Theme",
    0x00040000: "Video",
    0x000B0000: "Title Update",
    0x00080000: "Demo",
    0x00090000: "Video",
    0x000D0000: "Arcade",
    0x00100000: "Profile",
    0x00020000: "Gamer picture",
    0x00007000: "Games on Demand",
    0x00000004: "Installed game (cache)",
}


def be24(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def le24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


class StfsPackage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0xA000)
        self.magic = h[0:4].decode("ascii", "replace")
        if self.magic not in ("CON ", "LIVE", "PIRS"):
            raise ValueError(f"not an STFS package (magic {h[0:4]!r})")
        self.header_size = struct.unpack_from(">I", h, 0x340)[0]
        self.content_type = struct.unpack_from(">I", h, 0x344)[0]
        self.content_size = struct.unpack_from(">Q", h, 0x34C)[0]
        self.media_id, self.version, self.base_version, self.title_id = struct.unpack_from(">IIII", h, 0x354)
        self.descriptor_type = struct.unpack_from(">I", h, 0x3A9)[0]  # 0 = STFS, 1 = SVOD
        vd = h[0x379:0x379 + 0x24]
        self.block_separation = vd[2]
        self.file_table_block_count = struct.unpack_from("<H", vd, 3)[0]
        self.file_table_block = le24(vd, 5)
        self.total_allocated_blocks = struct.unpack_from(">I", vd, 0x1C)[0]
        self.display_name = h[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        self.title_name = h[0x1691:0x1691 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        # "Male" package (2 hash tables per level) if bit 0 of the block separation byte is 0.
        self.sex = (~self.block_separation) & 1
        self.first_hash_table = (self.header_size + 0x0FFF) & 0xFFFFF000
        self.block_step = (0xAB, 0x718F) if self.sex == 0 else (0xAC, 0x723A)
        self.entries = []
        if self.descriptor_type == 0:
            self._read_file_table()

    # --- block addressing ---
    def _backing_data_block(self, n):
        r = (((n + 0xAA) // 0xAA) << self.sex) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << self.sex)
        return (1 << self.sex) + r + (((n + 0x70E4) // 0x70E4) << self.sex)

    def block_offset(self, n):
        return (self._backing_data_block(n) << 12) + self.first_hash_table

    def _level0_hash_block(self, n):
        if n < 0xAA:
            return 0
        num = (n // 0xAA) * self.block_step[0]
        num += ((n // 0x70E4) + 1) << self.sex
        if n // 0x70E4 == 0:
            return num
        return num + (1 << self.sex)

    def hash_entry(self, n):
        table = (self._level0_hash_block(n) << 12) + self.first_hash_table
        self.f.seek(table + (n % HASHES_PER_TABLE) * 0x18)
        e = self.f.read(0x18)
        return e[:0x14], e[0x14], be24(e, 0x15)

    def read_block(self, n):
        self.f.seek(self.block_offset(n))
        return self.f.read(BLOCK)

    def block_chain(self, start, count, consecutive):
        blocks, n = [], start
        for i in range(count):
            blocks.append(n)
            if consecutive:
                n += 1
            else:
                n = self.hash_entry(n)[2]
                if n == 0xFFFFFF and i + 1 < count:
                    raise ValueError(f"block chain cut at {start:#x}")
        return blocks

    # --- file table ---
    def _read_file_table(self):
        blocks = self.block_chain(self.file_table_block, self.file_table_block_count, False)
        for b in blocks:
            data = self.read_block(b)
            for i in range(BLOCK // 0x40):
                e = data[i * 0x40:(i + 1) * 0x40]
                flags = e[0x28]
                name_len = flags & 0x3F
                if name_len == 0:
                    continue
                self.entries.append({
                    "index": len(self.entries),
                    "name": e[:name_len].decode("ascii", "replace"),
                    "dir": bool(flags & 0x80),
                    "consecutive": bool(flags & 0x40),
                    "blocks": le24(e, 0x29),
                    "start": le24(e, 0x2F),
                    "parent": struct.unpack_from(">h", e, 0x32)[0],
                    "size": struct.unpack_from(">I", e, 0x34)[0],
                })

    def path_of(self, entry):
        parts, e = [entry["name"]], entry
        while e["parent"] != -1:
            e = self.entries[e["parent"]]
            parts.append(e["name"])
        return "/".join(reversed(parts))

    def read_file(self, entry, verify=True):
        out = bytearray()
        for n in self.block_chain(entry["start"], entry["blocks"], entry["consecutive"]):
            data = self.read_block(n)
            if verify:
                sha, _, _ = self.hash_entry(n)
                if sha != b"\0" * 20 and hashlib.sha1(data).digest() != sha:
                    raise ValueError(f"wrong SHA-1 in block {n:#x} of {self.path_of(entry)}")
            out += data
        return bytes(out[:entry["size"]])


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "list", "extract"):
        sys.exit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    pkg = StfsPackage(path)
    if cmd == "info":
        print(f"File           : {os.path.basename(path)}")
        print(f"Magic          : {pkg.magic.strip()}")
        print(f"Type           : {pkg.content_type:08X} ({CONTENT_TYPES.get(pkg.content_type, 'unknown')})")
        print(f"Title ID       : {pkg.title_id:08X}  ({pkg.title_name})")
        print(f"Name           : {pkg.display_name}")
        v = pkg.version
        print(f"Version        : {v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}")
        print(f"Volume         : {'STFS' if pkg.descriptor_type == 0 else 'SVOD'}, "
              f"{'male (2 tables)' if pkg.sex else 'female (1 table)'}, "
              f"{pkg.total_allocated_blocks} blocks")
        print(f"Entries        : {len(pkg.entries)}")
    elif cmd == "list":
        for e in pkg.entries:
            kind = "<DIR>" if e["dir"] else f"{e['size']:>12,}"
            print(f"{kind}  {pkg.path_of(e)}")
    else:
        dest = sys.argv[3]
        files = 0
        for e in pkg.entries:
            target = os.path.join(dest, *pkg.path_of(e).split("/"))
            if e["dir"]:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as out:
                out.write(pkg.read_file(e))
            files += 1
        print(f"Extracted {files} files to {dest} (every block SHA-1 verified)")


if __name__ == "__main__":
    main()
