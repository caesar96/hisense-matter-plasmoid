#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Undo everything install.sh did: stop and remove the systemd user unit,
# uninstall the plasmoid and delete the installed binaries.
#
# The commissioned Matter fabric under ~/.local/share/hisenseac is kept by
# default so a re-install stays paired with the AC; pass --purge to remove it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

PURGE=0
for arg in "$@"; do
    case "$arg" in
        --purge) PURGE=1 ;;
        -h|--help)
            echo "usage: $0 [--purge]"
            echo "  --purge   also delete the commissioned state in $PREFIX/share/hisenseac"
            exit 0
            ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

echo ">> stopping daemon"
systemctl --user disable --now hisense-matterd.service >/dev/null 2>&1 || true
systemctl --user reset-failed hisense-matterd.service >/dev/null 2>&1 || true

echo ">> removing systemd user unit"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
rm -f "$UNIT_DIR/hisense-matterd.service"
systemctl --user daemon-reload || true

echo ">> removing plasmoid"
kpackagetool6 --type Plasma/Applet --remove org.kde.hisenseac >/dev/null 2>&1 || true

echo ">> removing binaries from $PREFIX/bin"
if [[ -f "$ROOT/build/install_manifest.txt" ]]; then
    while IFS= read -r path; do
        [[ -f "$path" ]] && rm -f "$path"
    done < "$ROOT/build/install_manifest.txt"
else
    rm -f "$PREFIX/bin/hisense-matterd" "$PREFIX/bin/hisense-ac"
fi

echo ">> removing packaged service copy"
rm -f "$PREFIX/share/hisenseac/hisense-matterd.service"
rmdir "$PREFIX/share/hisenseac" 2>/dev/null || true

if [[ "$PURGE" == "1" ]]; then
    echo ">> purging commissioned state ($PREFIX/share/hisenseac)"
    rm -rf "$PREFIX/share/hisenseac"
    echo
    echo "Done. The AC was unpaired; re-run provision.sh before reinstalling."
else
    echo
    echo "Done. Commissioned state kept in $PREFIX/share/hisenseac (use --purge to remove)."
fi
