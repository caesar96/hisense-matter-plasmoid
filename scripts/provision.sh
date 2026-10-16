#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Commission the Hisense AC into a *persistent* chip-tool fabric.
#
# The AC must be in pairing mode (typically: hold the WiFi button ~3s until the
# WiFi icon blinks). Prefer a 2.4 GHz network for this device.
#
# Usage:
#   scripts/provision.sh <ssid> <wifi-password> <manual-pairing-code> [node-id]
#
# The manual pairing code is the numeric code printed on / under the AC (or in
# the QR payload, decoded with: chip-tool payload parse-setup-payload "<code>").
set -euo pipefail

STORE="${HISENSEAC_STORE:-$HOME/.local/share/hisenseac/chip-tool}"
CHIP_TOOL="${HISENSEAC_CHIP_TOOL:-chip-tool}"

SSID="${1:-}"
PASS="${2:-}"
CODE="${3:-}"
NODE_ID="${4:-1}"

if [[ -z "$SSID" || -z "$PASS" || -z "$CODE" ]]; then
    echo "usage: $0 <ssid> <wifi-password> <manual-pairing-code> [node-id]" >&2
    exit 2
fi

command -v "$CHIP_TOOL" >/dev/null || { echo "chip-tool not found" >&2; exit 1; }

mkdir -p "$STORE"
# Start from a clean slate so a previous fabric can't interfere.
rm -f "$STORE"/chip_tool_config*.ini "$STORE"/chip_tool_kvs \
      "$STORE"/chip_config.ini "$STORE"/chip_factory.ini \
      "$STORE"/chip_counters.ini "$STORE/.commissioned"
echo ">> commissioning node $NODE_ID into $STORE"

# NOTE: chip-tool expects global options at the END of the argument list.
"$CHIP_TOOL" pairing code-wifi "$NODE_ID" "$SSID" "$PASS" "$CODE" \
    --bypass-attestation-verifier 1 \
    --storage-directory "$STORE"

# Tell the daemon a device is paired (it keys off this marker, not chip-tool's
# fabric keys, which always include the controller's own fabric).
printf '1' > "$STORE/.commissioned"

echo ">> done. Verify with: hisense-ac status"
