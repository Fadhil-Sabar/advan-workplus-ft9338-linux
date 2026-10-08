#!/usr/bin/env python3
"""Extract FT9338W firmware from a libfprint ELF shared object.

Usage: extract-firmware.py <path-to-libfprint-2.so> [out.h]
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SYM = "FOCALFP_9338_FW_APP"


class ExtractionError(Exception):
    """Invalid input ELF, symbol table, or firmware section."""


def _fail(message: str) -> ExtractionError:
    return ExtractionError(message)


def read_firmware(path: Path, nm: str = "nm") -> bytes:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise _fail(f"cannot read {path}: {exc}") from exc

    # Require ELF64 little-endian, either common ELF machine; don't treat an
    # arbitrary file or a differently encoded ELF as section-header data.
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise _fail(f"{path} is not a complete ELF file")
    if data[4] != 2 or data[5] != 1 or data[6] != 1:
        raise _fail(f"{path} must be a current, little-endian ELF64 file")

    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type not in (2, 3):
        raise _fail(f"{path} is not an executable or shared ELF object")
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHHH", data, 52)
    if e_ehsize < 64 or e_shentsize < 64 or e_shnum == 0 or e_shstrndx >= e_shnum:
        raise _fail(f"{path} has invalid or unsupported ELF section-header metadata")
    if e_shoff > len(data) or e_shnum > (len(data) - e_shoff) // e_shentsize:
        raise _fail(f"{path} section-header table is truncated")

    try:
        result = subprocess.run([nm, "-S", str(path)], capture_output=True,
                                text=True, check=False)
    except OSError as exc:
        raise _fail(f"could not run {nm}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit status {result.returncode}"
        raise _fail(f"{nm} failed for {path}: {detail}")

    matches: list[tuple[int, int]] = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields or fields[-1] != SYM:
            continue
        # nm -S formats symbol value, size, type, name. Reject malformed or
        # ambiguous reports instead of silently using the last matching row.
        if len(fields) < 4:
            raise _fail(f"malformed nm record for {SYM}: {line!r}")
        try:
            value, size = int(fields[0], 16), int(fields[1], 16)
        except ValueError as exc:
            raise _fail(f"invalid nm address/size for {SYM}: {line!r}") from exc
        if size <= 0:
            raise _fail(f"symbol {SYM} has zero length")
        matches.append((value, size))
    if len(matches) != 1:
        state = "not found" if not matches else "ambiguous"
        raise _fail(f"symbol {SYM} {state} in {path} (wrong/updated blob?)")
    vaddr, size = matches[0]

    sections: list[tuple[int, int, int, int]] = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", data, off + 4)[0]
        sh_addr, sh_offset, sh_size = struct.unpack_from("<QQQ", data, off + 16)
        if sh_type != 8 and sh_size:  # SHT_NOBITS has no file-backed payload
            if sh_offset > len(data) or sh_size > len(data) - sh_offset:
                raise _fail(f"section {i} extends past end of ELF file")
        if sh_addr <= vaddr and vaddr < sh_addr + sh_size:
            sections.append((sh_addr, sh_offset, sh_size, sh_type))
    if len(sections) != 1:
        raise _fail(f"symbol {SYM} maps to {len(sections)} ELF sections; expected exactly one")
    sh_addr, sh_offset, sh_size, sh_type = sections[0]
    delta = vaddr - sh_addr
    if sh_type == 8 or delta > sh_size or size > sh_size - delta:
        raise _fail(f"symbol {SYM} length extends beyond its file-backed ELF section")
    file_offset = sh_offset + delta
    if file_offset > len(data) or size > len(data) - file_offset:
        raise _fail(f"symbol {SYM} payload extends past end of ELF file")
    firmware = data[file_offset:file_offset + size]
    if len(firmware) != size:
        raise _fail(f"short read extracting {SYM}: expected {size}, got {len(firmware)}")
    return firmware


def write_header(firmware: bytes, output: Path) -> None:
    if not firmware:
        raise _fail("refusing to write empty firmware")
    output.parent.mkdir(parents=True, exist_ok=True)
    content = [
        "/* FT9338W MCU firmware - extracted from ft9201-static (FOCALFP_9338_FW_APP).",
        " * FocalTech proprietary; generated at build time, not committed. */",
        "static const guint8 ft9201_fw_data[] = {",
    ]
    for i in range(0, len(firmware), 12):
        content.append("  " + ", ".join(f"0x{byte:02x}" for byte in firmware[i:i + 12]) + ",")
    content.extend(("};", f"static const gsize ft9201_fw_size = {len(firmware)}U;", ""))
    fd, temp_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as stream:
            stream.write("\n".join(content))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, output)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str]) -> int:
    source = Path(argv[1]) if len(argv) > 1 else Path("blobs/ft9201-static/libfprint-2.so.2.0.0")
    output = Path(argv[2]) if len(argv) > 2 else Path("src/ft9201_fw.h")
    try:
        firmware = read_firmware(source)
        write_header(firmware, output)
    except (ExtractionError, OSError) as exc:
        print(f"extract-firmware.py: {exc}", file=sys.stderr)
        return 1
    print(f"wrote {output}: {len(firmware)} bytes from {SYM}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
