#!/usr/bin/env python3
"""Lay a PE file out by RVA so it can be mapped W^X-safe at runtime."""
import struct
import sys


class InvalidPE(ValueError):
    pass


def need(data, offset, size, what):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise InvalidPE(f"truncated or out-of-range {what}")


def unpack(data, fmt, offset, what):
    size = struct.calcsize(fmt)
    need(data, offset, size, what)
    return struct.unpack_from(fmt, data, offset)[0]


def prepare(raw):
    need(raw, 0, 0x40, "DOS header")
    if unpack(raw, "<H", 0, "DOS signature") != 0x5A4D:
        raise InvalidPE("invalid DOS signature")
    pe = unpack(raw, "<I", 0x3C, "PE header offset")
    need(raw, pe, 24, "PE/COFF headers")
    if unpack(raw, "<I", pe, "PE signature") != 0x00004550:
        raise InvalidPE("invalid PE signature")
    machine = unpack(raw, "<H", pe + 4, "machine")
    nsec = unpack(raw, "<H", pe + 6, "section count")
    optsz = unpack(raw, "<H", pe + 20, "optional header size")
    opt = pe + 24
    need(raw, opt, optsz, "optional header")
    if machine != 0x8664 or optsz < 112 or unpack(raw, "<H", opt, "optional header magic") != 0x20B:
        raise InvalidPE("expected PE32+ x86-64 image")
    image_size = unpack(raw, "<I", opt + 56, "SizeOfImage")
    header_size = unpack(raw, "<I", opt + 60, "SizeOfHeaders")
    if not image_size or not header_size or header_size > image_size or header_size > len(raw):
        raise InvalidPE("invalid image/header size")
    sectbl = opt + optsz
    need(raw, sectbl, nsec * 40, "section table")
    sections = []
    for index in range(nsec):
        section = sectbl + index * 40
        vsize = unpack(raw, "<I", section + 8, "section virtual size")
        va = unpack(raw, "<I", section + 12, "section RVA")
        raw_size = unpack(raw, "<I", section + 16, "section raw size")
        raw_offset = unpack(raw, "<I", section + 20, "section raw offset")
        mapped_size = max(vsize, raw_size)
        if va > image_size or mapped_size > image_size - va:
            raise InvalidPE("section exceeds SizeOfImage")
        need(raw, raw_offset, raw_size, "section raw data")
        sections.append((va, raw_size, raw_offset))

    image = bytearray(image_size)
    image[:header_size] = raw[:header_size]
    for va, raw_size, raw_offset in sections:
        image[va:va + raw_size] = raw[raw_offset:raw_offset + raw_size]
    return image


def main():
    if len(sys.argv) != 3:
        raise InvalidPE("usage: prepare-engine-image.py INPUT OUTPUT")
    src, dst = sys.argv[1:]
    with open(src, "rb") as source:
        image = prepare(source.read())
    with open(dst, "wb") as output:
        output.write(image)
    print(f"prepared {dst}: {len(image)} bytes")


if __name__ == "__main__":
    try:
        main()
    except (InvalidPE, OSError, struct.error) as exc:
        print(f"prepare-engine-image.py: {exc}", file=sys.stderr)
        sys.exit(1)
