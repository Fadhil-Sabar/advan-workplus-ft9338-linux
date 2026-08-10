#!/usr/bin/env bash
# Fetch the two proprietary FocalTech pieces this driver needs, from existing
# public sources. Nothing proprietary is committed to this repo.
#
#   1. ftWbioEngineAdapter.dll  — the matcher, from FocalTech's signed Windows
#      driver on the Microsoft Update Catalog.
#   2. FT9338W MCU firmware     — extracted from the public ft9201-static blob.
#
# If either URL has moved, see the "Sourcing the blobs by hand" section of the
# README: you only need those two files, and this script shows exactly what they
# are and where they go.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p blobs

# --- 1. vendor matcher DLL (Microsoft Update Catalog) ---
DLL_CAB="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab"
if [ ! -f blobs/ftWbioEngineAdapter.dll ]; then
  echo "==> fetching FocalTech Windows driver (for ftWbioEngineAdapter.dll)"
  curl -fSL -o blobs/ftfocal.cab "$DLL_CAB"
  command -v cabextract >/dev/null || { echo "!! need 'cabextract' (dnf/apt install cabextract)"; exit 1; }
  cabextract -q -d blobs blobs/ftfocal.cab
fi
[ -f blobs/ftWbioEngineAdapter.dll ] && echo "    ok: blobs/ftWbioEngineAdapter.dll"

# --- 2. MCU firmware (ft9201-static public blob) ---
FW_DEB="https://github.com/mrrbrilliant/ft9201-static/raw/main/libfprint_2_2_1_90_1%2Btod1_0ubuntu120_04_2_amd64_16c6e64404f8411.deb"
if [ ! -f src/ft9201_fw.h ]; then
  echo "==> fetching ft9201-static blob (for FT9338W MCU firmware)"
  curl -fSL -o blobs/ft9201-static.deb "$FW_DEB"
  ( cd blobs && ar x ft9201-static.deb && tar xf data.tar.* )
  SO="$(find blobs -name 'libfprint-2.so.2.0.0' | head -1)"
  [ -n "$SO" ] || { echo "!! could not find libfprint .so inside the .deb"; exit 1; }
  python3 scripts/extract-firmware.py "$SO" src/ft9201_fw.h
fi
[ -f src/ft9201_fw.h ] && echo "    ok: src/ft9201_fw.h"

echo "Done. Next: scripts/build.sh"
