#!/usr/bin/env bash
# Fetch the proprietary matcher and public libfprint source used to extract MCU
# firmware. Downloads and generated files are staged and only published after
# successful validation/extraction.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p blobs src

# These hashes are from the repository's pre-existing, locally sourced CAB and
# extracted matcher (sha256sum of blobs/ftfocal.cab and
# blobs/ftWbioEngineAdapter.dll on 2026-06-22). They are provenance checks for
# that exact known release, not values inferred from the URLs.
DLL_CAB="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab"
DLL_CAB_SHA256=65dffda7ddc76d30b324ebf02c989cb74d0ec85ceeac3b1e10d37fcc3d321811
DLL_SHA256=ca34ba1e8494fc84a9ac31dd5c003e825d847484e5e3509f099de123d7953c6a
FW_DEB="https://github.com/mrrbrilliant/ft9201-static/raw/main/libfprint_2_2_1_90_1%2Btod1_0ubuntu120_04_2_amd64_16c6e64404f8411.deb"

work=$(mktemp -d "${TMPDIR:-/tmp}/ft9201-fetch.XXXXXXXX")
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Matcher DLL: preserve an existing validated file; otherwise extract into a
# private directory and atomically install only the expected, hash-verified DLL.
if [[ -f blobs/ftWbioEngineAdapter.dll ]]; then
  actual=$(sha256sum blobs/ftWbioEngineAdapter.dll | awk '{print $1}')
  [[ "$actual" == "$DLL_SHA256" ]] || { echo "!! existing matcher DLL hash mismatch" >&2; exit 1; }
  echo "    ok: blobs/ftWbioEngineAdapter.dll"
else
  echo "==> fetching FocalTech Windows driver (for ftWbioEngineAdapter.dll)"
  curl -fSL --output "$work/ftfocal.cab" "$DLL_CAB"
  actual=$(sha256sum "$work/ftfocal.cab" | awk '{print $1}')
  [[ "$actual" == "$DLL_CAB_SHA256" ]] || { echo "!! downloaded CAB hash mismatch" >&2; exit 1; }
  command -v cabextract >/dev/null || { echo "!! need 'cabextract' (dnf/apt install cabextract)" >&2; exit 1; }
  mkdir "$work/cab"
  cabextract -q -d "$work/cab" "$work/ftfocal.cab"
  dll=$(find "$work/cab" -type f -name ftWbioEngineAdapter.dll -print -quit)
  [[ -n "$dll" ]] || { echo "!! CAB did not contain ftWbioEngineAdapter.dll" >&2; exit 1; }
  actual=$(sha256sum "$dll" | awk '{print $1}')
  [[ "$actual" == "$DLL_SHA256" ]] || { echo "!! extracted matcher DLL hash mismatch" >&2; exit 1; }
  # Stage both in the destination filesystem; publish CAB as a harmless
  # auxiliary artifact first and the required DLL last as the completion marker.
  cab_stage=$(mktemp blobs/.ftfocal.cab.XXXXXXXX)
  dll_stage=$(mktemp blobs/.ftWbioEngineAdapter.dll.XXXXXXXX)
  install -m 0644 "$work/ftfocal.cab" "$cab_stage"
  install -m 0644 "$dll" "$dll_stage"
  mv -f "$cab_stage" blobs/ftfocal.cab
  mv -f "$dll_stage" blobs/ftWbioEngineAdapter.dll
fi

# Firmware source is fetched/extracted afresh unless output already exists.
if [[ ! -s src/ft9201_fw.h ]]; then
  echo "==> fetching ft9201-static blob (for FT9338W MCU firmware)"
  curl -fSL --output "$work/ft9201-static.deb" "$FW_DEB"
  command -v ar >/dev/null && command -v tar >/dev/null || { echo "!! need ar and tar" >&2; exit 1; }
  mkdir "$work/deb"
  (cd "$work/deb" && ar x ../ft9201-static.deb)
  data_tar=$(find "$work/deb" -maxdepth 1 -type f -name 'data.tar.*' -print -quit)
  [[ -n "$data_tar" ]] || { echo "!! downloaded .deb has no data archive" >&2; exit 1; }
  mkdir "$work/root"
  tar -xf "$data_tar" -C "$work/root"
  so=$(find "$work/root" -type f -name 'libfprint-2.so.2.0.0' -print -quit)
  [[ -n "$so" ]] || { echo "!! could not find libfprint .so inside the .deb" >&2; exit 1; }
  python3 scripts/extract-firmware.py "$so" "$work/ft9201_fw.h"
  [[ -s "$work/ft9201_fw.h" ]] || { echo "!! firmware extraction produced an empty header" >&2; exit 1; }
  mv -f "$work/ft9201_fw.h" src/ft9201_fw.h
else
  echo "    ok: src/ft9201_fw.h (already present)"
fi

echo "Done. Next: scripts/build.sh"
