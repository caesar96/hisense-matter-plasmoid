#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build and install the daemon, CLI client, systemd user unit and plasmoid
# into the user's local prefix (~/.local).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

echo ">> configuring"
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null

echo ">> building"
cmake --build "$ROOT/build"

echo ">> installing binaries into $PREFIX/bin"
cmake --install "$ROOT/build" >/dev/null

echo ">> installing plasmoid"
if kpackagetool6 --type Plasma/Applet --upgrade "$ROOT/plasmoid/org.kde.hisenseac" >/dev/null 2>&1; then
    echo "   upgraded existing plasmoid"
else
    kpackagetool6 --type Plasma/Applet --install "$ROOT/plasmoid/org.kde.hisenseac"
fi

mkdir -p "$PREFIX/share/hisenseac"
cp "$ROOT/packaging/hisense-matterd.service" "$PREFIX/share/hisenseac/"
cp "$ROOT/packaging/hisense-connectlife.service" "$PREFIX/share/hisenseac/"
cp "$ROOT/packaging/connectlife.conf.example" "$PREFIX/share/hisenseac/"

echo ">> installing systemd user units"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
mkdir -p "$UNIT_DIR"
cp "$ROOT/packaging/hisense-matterd.service" "$UNIT_DIR/"
cp "$ROOT/packaging/hisense-connectlife.service" "$UNIT_DIR/"
systemctl --user daemon-reload || true

CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hisenseac"
if [[ ! -f "$CONFIG_DIR/connectlife.conf" ]]; then
    mkdir -p "$CONFIG_DIR"
    install -m600 "$ROOT/packaging/connectlife.conf.example" "$CONFIG_DIR/connectlife.conf"
    echo ">> seeded $CONFIG_DIR/connectlife.conf (fill in your ConnectLife login)"
fi

echo
echo "Done. Next steps:"
echo "  1. make sure ~/.local/bin is on your PATH"
echo "  2. commission the AC (if you have not already):"
echo "       $ROOT/scripts/provision.sh <ssid> <wifi-pass> <pairing-code>"
echo "  3. enable the daemon(s):"
echo "       systemctl --user enable --now hisense-matterd"
echo "       systemctl --user enable --now hisense-connectlife"
echo "  4. for the cloud daemon, put your ConnectLife login in:"
echo "       $CONFIG_DIR/connectlife.conf"
echo "  5. add the 'Hisense AC' widget to your panel/desktop"
