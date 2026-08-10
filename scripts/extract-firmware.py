#!/usr/bin/env python3
# Extract the FT9348W MCU firmware from FocalTech's public ft9201-static
# libfprint blob and emit src/ft9201_fw.h. The firmware is FocalTech's, so it
# is NOT committed to this repo — it is pulled from an existing public source
# at build time (see scripts/fetch-blobs.sh).
#
# Usage: extract-firmware.py <path-to-libfprint-2.so> [out.h]
import struct, subprocess, sys

SYM = "FOCALFP_9338_FW_APP"   # matches the FT9338W variant (device 2808:9338)

so  = sys.argv[1] if len(sys.argv) > 1 else "blobs/ft9201-static/libfprint-2.so.2.0.0"
out = sys.argv[2] if len(sys.argv) > 2 else "src/ft9201_fw.h"

data = open(so, "rb").read()

# 1. symbol virtual address + size (binutils nm is universally available)
vaddr = size = None
for line in subprocess.run(["nm", "-S", so], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) >= 4 and p[-1] == SYM:
        vaddr, size = int(p[0], 16), int(p[1], 16)
if vaddr is None:
    sys.exit(f"symbol {SYM} not found in {so} (wrong/updated blob?)")

# 2. map vaddr -> file offset via ELF64 section headers
e_shoff    = struct.unpack_from("<Q", data, 0x28)[0]
e_shentsz  = struct.unpack_from("<H", data, 0x3a)[0]
e_shnum    = struct.unpack_from("<H", data, 0x3c)[0]
foff = None
for i in range(e_shnum):
    sh = e_shoff + i * e_shentsz
    sh_addr = struct.unpack_from("<Q", data, sh + 16)[0]
    sh_off  = struct.unpack_from("<Q", data, sh + 24)[0]
    sh_size = struct.unpack_from("<Q", data, sh + 32)[0]
    if sh_addr and sh_addr <= vaddr < sh_addr + sh_size:
        foff = sh_off + (vaddr - sh_addr)
        break
if foff is None:
    sys.exit("could not map symbol vaddr to a file offset")

fw = data[foff:foff + size]

with open(out, "w") as f:
    f.write(f"/* FT9338W MCU firmware — extracted from ft9201-static ({SYM}).\n")
    f.write(" * FocalTech proprietary; generated at build time, not committed. */\n")
    f.write("static const guint8 ft9201_fw_data[] = {\n")
    for i in range(0, len(fw), 12):
        f.write("  " + ", ".join("0x%02x" % b for b in fw[i:i+12]) + ",\n")
    f.write("};\n")
    f.write(f"static const gsize ft9201_fw_size = {len(fw)}U;\n")

print(f"wrote {out}: {len(fw)} bytes from {SYM}")
