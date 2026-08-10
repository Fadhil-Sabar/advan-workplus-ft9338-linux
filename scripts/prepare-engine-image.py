#!/usr/bin/env python3
"""Lay a PE file out by RVA so it can be mapped W^X-safe at runtime."""
import struct
import sys

src, dst = sys.argv[1:3]
raw = open(src, "rb").read()
pe = struct.unpack_from("<I", raw, 0x3C)[0]
nsec = struct.unpack_from("<H", raw, pe + 6)[0]
optsz = struct.unpack_from("<H", raw, pe + 20)[0]
opt = pe + 24
image_size = struct.unpack_from("<I", raw, opt + 56)[0]
header_size = struct.unpack_from("<I", raw, opt + 60)[0]
sections = pe + 24 + optsz

image = bytearray(image_size)
image[:header_size] = raw[:header_size]
for index in range(nsec):
    section = sections + index * 40
    virtual_address = struct.unpack_from("<I", raw, section + 12)[0]
    raw_size = struct.unpack_from("<I", raw, section + 16)[0]
    raw_offset = struct.unpack_from("<I", raw, section + 20)[0]
    image[virtual_address:virtual_address + raw_size] = raw[raw_offset:raw_offset + raw_size]

with open(dst, "wb") as output:
    output.write(image)
print(f"prepared {dst}: {len(image)} bytes")
