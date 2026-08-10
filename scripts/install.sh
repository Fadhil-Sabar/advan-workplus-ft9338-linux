#!/usr/bin/env bash
# Install the FT9201-enabled libfprint side-by-side and point fprintd at it.
# The distro's own libfprint is left untouched. No security hardening is
# disabled — the driver's loader is W^X-safe, so fprintd's default
# MemoryDenyWriteExecute stays on.
set -euo pipefail
cd "$(dirname "$0")/.."

DEST=/usr/local/lib/ft9201
DROPIN=/etc/systemd/system/fprintd.service.d/ft9201.conf

if [ "${1:-}" = "--uninstall" ]; then
  [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
  rm -rf "$DEST"
  rm -f "$DROPIN" /usr/lib/libfprint-2/ftWbioEngineAdapter.dll \
    /usr/lib/libfprint-2/ftWbioEngineAdapter.dll.image \
    /etc/udev/rules.d/60-ft9201.rules
  systemctl daemon-reload || true; systemctl restart fprintd || true
  udevadm control --reload-rules || true
  echo "Uninstalled. The distro libfprint was never modified."
  exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
SO=libfprint/build/libfprint/libfprint-2.so.2.0.0
[ -f "$SO" ] || { echo "!! build first: scripts/build.sh"; exit 1; }
[ -f blobs/ftWbioEngineAdapter.dll ] || { echo "!! run scripts/fetch-blobs.sh first"; exit 1; }

echo "==> installing libfprint (with FT9201) to $DEST"
install -d "$DEST"
install -m755 "$SO" "$DEST/libfprint-2.so.2.0.0"
ln -sf libfprint-2.so.2.0.0 "$DEST/libfprint-2.so.2"

echo "==> installing vendor matcher DLL"
install -Dm644 blobs/ftWbioEngineAdapter.dll /usr/lib/libfprint-2/ftWbioEngineAdapter.dll
python3 scripts/prepare-engine-image.py \
  blobs/ftWbioEngineAdapter.dll blobs/ftWbioEngineAdapter.dll.image
install -m644 blobs/ftWbioEngineAdapter.dll.image \
  /usr/lib/libfprint-2/ftWbioEngineAdapter.dll.image
restorecon -F /usr/lib/libfprint-2/ftWbioEngineAdapter.dll \
  /usr/lib/libfprint-2/ftWbioEngineAdapter.dll.image 2>/dev/null || true

echo "==> installing udev rule"
install -Dm644 packaging/60-ft9201.rules /etc/udev/rules.d/60-ft9201.rules
udevadm control --reload-rules && udevadm trigger || true

echo "==> pointing fprintd at our libfprint (distro libfprint untouched)"
install -d "$(dirname "$DROPIN")"
cat > "$DROPIN" <<EOF
[Service]
Environment=LD_LIBRARY_PATH=$DEST
Environment=FT9201_ENGINE_DLL=/usr/lib/libfprint-2/ftWbioEngineAdapter.dll
EOF
systemctl daemon-reload
systemctl restart fprintd || true

echo
echo "Installed. Enroll with:  fprintd-enroll"
echo "  (or KDE System Settings > Users > Configure Fingerprint Authentication)"
echo "Undo everything with:    sudo scripts/install.sh --uninstall"
