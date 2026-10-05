"""Decrypts and decompresses a retail XEX2 (AES encryption + 'basic' compression) and dumps the
in-memory image exactly as the console loads it.

Usage: python tools/xex_extract.py <input.xex> <output.bin>
The virtual address of each byte is image_base + its offset in the output file.
"""
import struct
import sys

from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")


def main(src, dst):
    data = open(src, "rb").read()
    magic, _, pe_off, _, sec_off, nopt = struct.unpack(">4sIIIII", data[:24])
    assert magic == b"XEX2", "not an XEX2"

    headers = dict(struct.unpack(">II", data[24 + i * 8:32 + i * 8]) for i in range(nopt))
    ffi = headers[0x000003FF]
    info_size, enc, comp = struct.unpack(">IHH", data[ffi:ffi + 8])

    image_size = struct.unpack(">I", data[sec_off + 4:sec_off + 8])[0]
    load_address = struct.unpack(">I", data[sec_off + 0x110:sec_off + 0x114])[0]
    payload = data[pe_off:]

    if enc == 1:
        enc_key = data[sec_off + 0x150:sec_off + 0x160]
        session_key = AES.new(RETAIL_KEY, AES.MODE_ECB).decrypt(enc_key)
        usable = len(payload) - len(payload) % 16
        payload = AES.new(session_key, AES.MODE_CBC, iv=bytes(16)).decrypt(payload[:usable])

    if comp == 0:
        image = payload[:image_size]
    elif comp == 1:
        out = bytearray()
        pos = 0
        for i in range((info_size - 8) // 8):
            dsize, zsize = struct.unpack(">II", data[ffi + 8 + i * 8:ffi + 16 + i * 8])
            out += payload[pos:pos + dsize]
            out += bytes(zsize)
            pos += dsize
        image = bytes(out)
    else:
        sys.exit(f"compression {comp} not supported (LZX/delta)")

    assert image[:2] == b"MZ", "the decrypted image does not start with MZ: wrong key or format"
    open(dst, "wb").write(image)
    print(f"OK: base=0x{load_address:08X} size=0x{len(image):X} -> {dst}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
