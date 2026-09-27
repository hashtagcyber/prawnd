#!/usr/bin/env bash
# Build and flash one of the bench bring-up tests, then open the serial monitor.
#
#   ./flash-test.sh            mictest   — both mics + LED level meter, with raw word dump
#   ./flash-test.sh board      boardtest — LED + SD + 10 s WAV + level meter
#   ./flash-test.sh blink      blinktest — LED only
#   ./flash-test.sh -n ...     flash only, don't open the monitor
#
# The monitor needs a real terminal (it fails inside scripts/agents); Ctrl-C exits.
# When done, put the real firmware back with ./flash-main.sh.
# The USB port is auto-detected (/dev/cu.usbmodem*); override with PORT=/dev/... .
set -euo pipefail
cd "$(dirname "$0")"

ENV=mictest
MONITOR=1
for a in "$@"; do
  case "$a" in
    mic|mictest)     ENV=mictest ;;
    board|boardtest) ENV=boardtest ;;
    blink|blinktest) ENV=blinktest ;;
    -n|--no-monitor) MONITOR=0 ;;
    -h|--help)       sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
if [ -z "$PORT" ]; then
  echo "No /dev/cu.usbmodem* device found. Plug the board in (replug if it was light-sleeping)." >&2
  exit 1
fi

echo "==> flashing test '$ENV' to $PORT"
pio run -e "$ENV" -t upload --upload-port "$PORT"

if [ "$MONITOR" = 1 ]; then
  echo "==> monitor on $PORT (Ctrl-C to exit; ./flash-main.sh restores the real firmware)"
  pio device monitor -p "$PORT" -b 115200
fi
