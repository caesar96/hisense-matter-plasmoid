#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Adopt an existing chip-tool commissioning whose state still lives in the
# default volatile location ($TMPDIR or /tmp) into a persistent store.
#
# chip-tool keeps the commissioner identity across TWO files:
#   chip_tool_config.ini        - fabric table (f/1/... keys)
#   chip_tool_config.alpha.ini  - commissioner CA/ICA keys (ExampleOpCreds*)
# Both (plus the empty chip_tool_kvs) must be copied or the daemon adds a new
# fabric and the AC no longer recognises the controller.
set -euo pipefail

STORE="${HISENSEAC_STORE:-$HOME/.local/share/hisenseac/chip-tool}"
SRC="${1:-${TMPDIR:-/tmp}}"

mkdir -p "$STORE"

shopt -s nullglob
found=0
for f in "$SRC"/chip_tool_config.ini \
         "$SRC"/chip_tool_config.alpha.ini \
         "$SRC"/chip_tool_kvs \
         "$SRC"/chip_config.ini \
         "$SRC"/chip_factory.ini \
         "$SRC"/chip_counters.ini; do
    if [[ -f "$f" ]]; then
        cp -v "$f" "$STORE/"
        found=1
    fi
done

if [[ "$found" -eq 0 ]]; then
    echo "no chip-tool state found in $SRC" >&2
    exit 1
fi

echo ">> imported into $STORE"
echo ">> NOTE: chip-tool resolves some paths literally under /tmp; keep $SRC"
echo ">>       around or re-run scripts/provision.sh for a self-contained store."
