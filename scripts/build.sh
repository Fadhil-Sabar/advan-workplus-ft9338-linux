#!/usr/bin/env bash
# Build libfprint with the FT9201 driver grafted in. We do NOT fork libfprint:
# this clones a pinned upstream release, drops our driver files into it, adds one
# line to its build config, and compiles. The repo only holds our driver.
set -euo pipefail
cd "$(dirname "$0")/.."

LIBFPRINT_REF="${LIBFPRINT_REF:-v1.94.10}"   # pinned, tested upstream release
LFP=libfprint

[ -f src/ft9201_fw.h ] || { echo "!! run scripts/fetch-blobs.sh first (missing firmware)"; exit 1; }
# The matcher DLL is only required for enroll/verify at runtime. Building and
# hardware bring-up can proceed without it.

if [ ! -d "$LFP" ]; then
  echo "==> cloning upstream libfprint @ $LIBFPRINT_REF"
  git clone --depth 1 --branch "$LIBFPRINT_REF" \
      https://gitlab.freedesktop.org/libfprint/libfprint.git "$LFP"
elif [ ! -d "$LFP/.git" ]; then
  echo "!! $LFP exists but is not a git checkout; refusing to modify it" >&2
  exit 1
fi

if ! ACTUAL_REF=$(git -C "$LFP" rev-parse HEAD 2>/dev/null); then
  echo "!! cannot determine the commit of existing $LFP checkout" >&2
  exit 1
fi
if ! EXPECTED_REF=$(git -C "$LFP" rev-parse "${LIBFPRINT_REF}^{commit}" 2>/dev/null); then
  echo "!! cannot resolve LIBFPRINT_REF '$LIBFPRINT_REF' in $LFP" >&2
  echo "   Ensure the requested ref exists locally; checkout contents were not changed." >&2
  exit 1
fi
if [ "$ACTUAL_REF" != "$EXPECTED_REF" ]; then
  echo "!! existing $LFP checkout is at $ACTUAL_REF, not requested $LIBFPRINT_REF ($EXPECTED_REF)" >&2
  echo "   Refusing to modify it; checkout contents and user changes were left untouched." >&2
  exit 1
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
