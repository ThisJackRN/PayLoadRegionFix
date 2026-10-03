"""Extract only the exact upstream-tested RPX embedded in an official installer.

The outer WUHB contains an RPX ELF; its compressed sections use a big-endian
uncompressed-size word followed by zlib data. No firmware or console data is used.
"""
import hashlib
import pathlib
import struct
import sys
import zlib

EXPECTED = "1736574cf6c949557aed0c817eb1927e35a9b820"


def elf_sections(data, base):
    if data[base:base + 6] != b"\x7fELF\x01\x02":
        return []
    header = struct.unpack_from(">HHIIIIIHHHHHH", data, base + 16)
    offset, stride, count = header[5], header[10], header[11]
    if stride != 40 or count > 512:
        return []
    return [struct.unpack_from(">IIIIIIIIII", data, base + offset + i * stride)
            for i in range(count)]


def find_payload(data):
    start = 0
    while True:
        start = data.find(b"\x7fELF\x01\x02", start)
        if start < 0:
            return None
        try:
            sections = elf_sections(data, start)
            header = struct.unpack_from(">HHIIIIIHHHHHH", data, start + 16)
            end = max([header[5] + header[10] * header[11]] +
                      [s[4] + s[5] for s in sections if s[1] != 8])
            for length in range(end, min(end + 4096, len(data) - start) + 1):
                candidate = data[start:start + length]
                if hashlib.sha1(candidate).hexdigest() == EXPECTED:
                    return candidate
        except (struct.error, ValueError):
            pass
        start += 4


container = pathlib.Path(sys.argv[1]).read_bytes()
outer = container.find(b"\x7fELF\x01\x02")
if outer < 0:
    raise SystemExit("No RPX ELF found in official installer")
for section in elf_sections(container, outer):
    if section[1] == 8:
        continue
    data = container[outer + section[4]:outer + section[4] + section[5]]
    if section[2] & 0x08000000:
        size = struct.unpack_from(">I", data)[0]
        data = zlib.decompress(data[4:])
        if len(data) != size:
            raise SystemExit("Invalid compressed section size")
    payload = find_payload(data)
    if payload is not None:
        pathlib.Path(sys.argv[2]).write_bytes(payload)
        print(f"Verified payload: {EXPECTED} ({len(payload)} bytes)")
        break
else:
    raise SystemExit("Official installer does not contain the expected tested payload")
