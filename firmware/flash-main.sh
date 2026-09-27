#!/usr/bin/env bash
# Build and flash the production recorder firmware to the connected XIAO ESP32-C6.
#
#   ./flash-main.sh            flash the base env (xiao_esp32c6)
#   ./flash-main.sh batt       flash the battery-gauge variant (xiao_esp32c6_batt)
#   ./flash-main.sh -m         flash, then open the serial monitor (Ctrl-C to exit)
#
# The USB port is auto-detected (/dev/cu.usbmodem*); override with PORT=/dev/... .
set -euo pipefail
cd "$(dirname "$0")"

ENV=xiao_esp32c6
MONITOR=0
for a in "$@"; do
  case "$a" in
    batt)         ENV=xiao_esp32c6_batt ;;
    -m|--monitor) MONITOR=1 ;;
    -h|--help)    sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
if [ -z "$PORT" ]; then
  echo "No /dev/cu.usbmodem* device found. Plug the board in (replug if it was light-sleeping)." >&2
  exit 1
fi

echo "==> flashing '$ENV' to $PORT"
pio run -e "$ENV" -t upload --upload-port "$PORT"

if [ "$MONITOR" = 1 ]; then
  echo "==> monitor on $PORT (Ctrl-C to exit)"
  pio device monitor -p "$PORT" -b 115200
fi
