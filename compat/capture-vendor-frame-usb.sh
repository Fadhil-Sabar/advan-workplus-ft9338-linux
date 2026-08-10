#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR_LIB="${VENDOR_LIB:-/tmp/ft9201-static-extract/usr/lib/x86_64-linux-gnu}"
TRACE="$ROOT/compat/vendor-frame-usbmon.log"
RUN_LOG="$ROOT/compat/vendor-capture.log"

if [[ $EUID -ne 0 ]]; then
  echo "Run this script with sudo" >&2
  exit 1
fi

modprobe usbmon
if [[ ! -d /sys/kernel/debug/usb/usbmon ]]; then
  mount -t debugfs debugfs /sys/kernel/debug
fi

read -r bus dev < <(
  lsusb -d 2808:9338 |
    awk '{sub(/^0+/, "", $2); sub(/:$/, "", $4); sub(/^0+/, "", $4); print $2, $4; exit}'
)
if [[ -z ${bus:-} || -z ${dev:-} ]]; then
  echo "Sensor 2808:9338 was not found" >&2
  exit 2
fi

: >"$TRACE"
: >"$RUN_LOG"
timeout 18 cat "/sys/kernel/debug/usb/usbmon/${bus}u" >"$TRACE" &
monitor_pid=$!
sleep 0.2

echo "Touch and hold the fingerprint sensor when prompted..."
env \
  LD_PRELOAD="$ROOT/compat/libgusb-compat.so" \
  LD_LIBRARY_PATH="$VENDOR_LIB" \
  G_MESSAGES_DEBUG=all \
  timeout 15 "$ROOT/compat/vendor-capture" 2>&1 | tee "$RUN_LOG" || true

wait "$monitor_pid" || true
echo "Captured bus $bus, device $dev"
echo "USB trace: $TRACE"
echo "Run log:   $RUN_LOG"
