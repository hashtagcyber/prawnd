#!/usr/bin/env bash
# Reset the Prawnd board over USB (no physical button needed).
# The XIAO ESP32-C6's RST button can wedge native USB until a replug on macOS;
# the esptool USB reset sequence recovers cleanly, so use this instead.
# NOTE: the serial monitor must be detached first — this needs the port.
set -euo pipefail

PORT="${1:-/dev/cu.usbmodem101}"
PY="$(ls /opt/homebrew/Cellar/platformio/*/libexec/bin/python3* | head -1)"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"

"$PY" "$ESPTOOL" --chip esp32c6 --port "$PORT" --after hard_reset read_mac >/dev/null
echo "Reset sent to $PORT — reattach the monitor now to catch boot logs (firmware waits 3s)."
