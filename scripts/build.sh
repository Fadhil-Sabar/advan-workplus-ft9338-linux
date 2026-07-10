#!/usr/bin/env bash
# Build libfprint with the FT9201 driver grafted in. We do NOT fork libfprint:
# this clones a pinned upstream release, drops our driver files into it, adds one
# line to its build config, and compiles. The repo only holds our driver.
set -euo pipefail
cd "$(dirname "$0")/.."

LIBFPRINT_REF="${LIBFPRINT_REF:-v1.94.10}"   # pinned, tested upstream release
LFP=libfprint

[ -f src/ft9201_fw.h ] || { echo "!! run scripts/fetch-blobs.sh first (missing firmware)"; exit 1; }
[ -f blobs/ftWbioEngineAdapter.dll ] || { echo "!! run scripts/fetch-blobs.sh first (missing DLL)"; exit 1; }

if [ ! -d "$LFP" ]; then
  echo "==> cloning upstream libfprint @ $LIBFPRINT_REF"
  git clone --depth 1 --branch "$LIBFPRINT_REF" \
      https://gitlab.freedesktop.org/libfprint/libfprint.git "$LFP"
fi

echo "==> installing driver files into the tree"
cp src/ft9201.c src/ft_engine.c src/ft_engine.h src/ft9201_fw.h "$LFP/libfprint/drivers/"

echo "==> registering the driver in meson"
python3 - "$LFP/libfprint/meson.build" <<'PY'
import sys
f = sys.argv[1]; s = open(f).read()
if "'ft9201'" not in s:
    s = s.replace("driver_sources = {",
                  "driver_sources = {\n    'ft9201' : [ 'drivers/ft9201.c', 'drivers/ft_engine.c' ],", 1)
    open(f, "w").write(s)
    print("    injected ft9201 into driver_sources")
else:
    print("    ft9201 already present")
PY

echo "==> configuring + building (only the ft9201 driver)"
RECONF=""; [ -d "$LFP/build" ] && RECONF="--reconfigure"
meson setup "$LFP/build" "$LFP" $RECONF \
    -Ddrivers=ft9201 -Dgtk-examples=false -Ddoc=false -Dintrospection=false >/dev/null
ninja -C "$LFP/build" libfprint/libfprint-2.so.2.0.0 examples/enroll examples/verify

echo
echo "Built: $LFP/build/libfprint/libfprint-2.so.2.0.0"
echo "Test without installing:"
echo "  FT9201_ENGINE_DLL=\$PWD/blobs/ftWbioEngineAdapter.dll \\"
echo "  LD_LIBRARY_PATH=$LFP/build/libfprint $LFP/build/examples/enroll"
echo "Or install for KDE/login:  sudo scripts/install.sh"
