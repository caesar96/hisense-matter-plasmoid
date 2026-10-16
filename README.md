# hisense-matter-plasmoid

Control a **Hisense Matter air conditioner** from a KDE Plasma 6 desktop.

This repository contains:

- **`hisense-matterd`** – a native C++/Qt6 session-bus daemon that keeps one
  warm `chip-tool interactive` process alive and exposes the AC as a clean
  D-Bus service (`org.kde.hisenseac`).
- **`hisense-ac`** – a small CLI client for the daemon (also used by the widget).
- **`org.kde.hisenseac`** – a Plasma 6 plasmoid (QML) that shows the room
  temperature and lets you turn the unit on/off, change the mode and set the
  target temperature.

It works fully offline over the local Matter fabric – **no ConnectLife cloud or
phone app is involved**.

## Why a daemon wrapping `chip-tool`?

The AC speaks Matter. The only Matter client already installed on most Linux
systems is `chip-tool`, which is a *one-shot* command that boots the whole stack
and re-establishes a CASE session on every invocation (~3–4 s). By driving a
single long-lived `chip-tool interactive` session, the daemon answers in
~250 ms per attribute after the first read. The D-Bus interface keeps the QML
widget simple and the Matter heavy lifting out of process.

A future backend could link `connectedhomeip` directly (`chip::Controller`) and
drop the `chip-tool` dependency; the D-Bus contract would not change.

## Requirements

- Linux with KDE Plasma 6 (Qt 6.6+, KF6)
- CMake + Ninja + a C++17 compiler
- `chip-tool` (from [connectedhomeip](https://github.com/project-chip/connectedhomeip))
  on `PATH`
- `kpackagetool6` for installing the widget

## Build & install

```sh
scripts/install.sh
```

This builds both binaries into `~/.local/bin`, installs the plasmoid, and drops
a systemd **user** unit (`hisense-matterd.service`).

## Commissioning the AC

Put the AC in pairing mode (hold its WiFi button for ~3 s until the WiFi icon
blinks), prefer a **2.4 GHz** network, then:

```sh
scripts/provision.sh "<ssid>" "<wifi-password>" "<manual-pairing-code>"
```

The manual pairing code is the numeric code on the unit/QR. State is written to
a **persistent** store (`~/.local/share/hisenseac/chip-tool`) instead of the
default volatile `/tmp`, so the commissioning survives reboots.

Already paired with plain `chip-tool` (state in `/tmp`)? Adopt it with:

```sh
scripts/import-current.sh
```

## Running

```sh
systemctl --user enable --now hisense-matterd
hisense-ac status
```

CLI usage:

```
hisense-ac status [--json]     print current AC state
hisense-ac on | off            turn the AC on (last mode) / off
hisense-ac mode <mode>         off|auto|cool|heat|fan|dry
hisense-ac setpoint <celsius>  set the active setpoint
hisense-ac refresh             force a re-read from the device
```

Then add the **Hisense AC** widget to your panel or desktop.

## Uninstall

```sh
scripts/uninstall.sh            # keeps the commissioned state
scripts/uninstall.sh --purge    # also deletes ~/.local/share/hisenseac
```

This stops/disables the daemon, removes the systemd user unit, uninstalls the
plasmoid and deletes the installed binaries. The Matter fabric is kept by
default so a re-install stays paired; `--purge` removes it as well (you will
need to re-run `provision.sh` afterwards).

## Known limitations

- **No fan-speed control.** The unit does not expose the Matter FanControl
  cluster and Matter's Thermostat cluster has no `FanMode` attribute, so fan
  speed cannot be set over Matter. It is not controllable through this interface
  at all.
- **No swing or display-light control.** Enumeration shows endpoint 1 only
  implements `Identify`, `Groups`, `Descriptor`, `Thermostat` and
  `ThermostatUserInterfaceConfiguration`; the Thermostat cluster exposes only its
  19 standard attributes (`FeatureMap = 3`, Heat+Cool). Swing/oscillation and the
  physical panel display light are not modelled by Matter on this unit and are
  only reachable via the Hisense/ConnectLife app or the IR remote.
- **Only Cool and Heat actually work.** The thermostat reports `FeatureMap = 3`
  (Heat + Cool only). Writing `SystemMode = 8` (Dry) is rejected outright with a
  device `FAILURE`. Writing `Auto` (1) or `Fan-only` (7) is *accepted* — the
  device even reports the new value back on a fresh read — but the unit's
  firmware never acts on it and the physical AC keeps its previous mode. The
  widget therefore offers only **Cool** and **Heat**. (Off is handled by the
  power button.)
- **Attestation is bypassed** at commissioning time
  (`--bypass-attestation-verifier 1`) because no PAA/CD trust store is shipped.
  Fine for a self-owned device on a trusted LAN; tighten for anything else.
- Temperatures are reported/received in **degrees Celsius**.
- Setpoint writes are re-applied after power-on (the unit can revert a setpoint
  set while off).
- Single AC / single fabric per store.

## Repository layout

```
src/            C++ daemon + CLI client (ChipCli, Controller, main, client)
plasmoid/       Plasma 6 plasmoid (org.kde.hisenseac)
scripts/        provision / import-current / install helpers
packaging/      systemd user unit
docs/           architecture notes
```

See [`docs/architecture.md`](docs/architecture.md) for the D-Bus contract and
implementation details.

## License

MIT.
