# hisense-matter-plasmoid

Control a **Hisense air conditioner** from a KDE Plasma 6 desktop.

Two interchangeable backends are provided; the plasmoid is pinned to the
chosen backend in its settings. Only the selected daemon runs (the widget
starts/enables the selected one and stops/disables the other in the user
systemd instance):

- **Matter (local, `hisense-matterd`)** – talks to the unit directly over your
  LAN via Matter. Fully offline, no account. Exposes only what the unit's Matter
  Thermostat cluster offers: power, **Cool/Heat** and the setpoint. No fan speed,
  swing or Dry (see [limitations](#known-limitations)).
- **ConnectLife cloud (`hisense-connectlife`)** – talks to the same
  ConnectLife/Higyu cloud service the official phone app uses. Full control
  (**all modes including Dry and Fan-only, fan speed, swing, eco, super, sleep,
  fan-mute**) but requires your ConnectLife account credentials and an internet
  connection.

This repository contains:

- **`hisense-matterd`** – a native C++/Qt6 session-bus daemon that keeps one
  warm `chip-tool interactive` process alive and exposes the AC as a clean
  D-Bus service (`org.kde.hisenseac`).
- **`hisense-connectlife`** – a native C++/Qt6 session-bus daemon that speaks
  the ConnectLife cloud REST API and exposes the AC as
  `org.kde.hisenseac.connectlife`.
- **`hisense-ac`** – a small CLI client for the Matter daemon.
- **`org.kde.hisenseac`** – a Plasma 6 plasmoid (QML) that shows the room
  temperature and lets you control the unit through either backend.

The Matter backend works fully offline over the local fabric – **no ConnectLife
cloud or phone app is involved**; the cloud backend is optional.

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
- OpenSSL development headers (used by the cloud backend for request signing)
- `chip-tool` (from [connectedhomeip](https://github.com/project-chip/connectedhomeip))
  on `PATH` – Matter backend only
- `kpackagetool6` for installing the widget

## Build & install

```sh
scripts/install.sh
```

This builds all three binaries into `~/.local/bin`, installs the plasmoid, drops
two systemd **user** units (`hisense-matterd.service` and
`hisense-connectlife.service`), and seeds an empty
`~/.config/hisenseac/connectlife.conf` (mode `600`) for the cloud backend.

## Commissioning the AC

Put the AC in pairing mode (hold its WiFi button for ~3 s until the WiFi icon
blinks), prefer a **2.4 GHz** network, then:

```sh
scripts/provision.sh "<ssid>" "<wifi-password>" "<manual-pairing-code>"
```

The manual pairing code is the numeric code on the unit/QR. State is written to
a **persistent** store (`~/.local/share/hisenseac/chip-tool`) instead of the
default volatile `/tmp`, so the commissioning survives reboots.

You can also pair straight from the widget: while the AC is not connected the
expanded popup shows a **Connect the air conditioner** form (SSID, Wi-Fi
password, pairing code). Those values are only used for this one-time pairing
and are never written to disk.

CLI equivalent:

```sh
hisense-ac provision "<ssid>" "<wifi-password>" "<pairing-code>"
```


Already paired with plain `chip-tool` (state in `/tmp`)? Adopt it with:

```sh
scripts/import-current.sh
```

> **Careful:** a bare `chip-tool` command writes its controller state to the
> default `/tmp` location, which is forgotten on reboot and does **not** match
> the daemon's fabric. If you drive `chip-tool` yourself, always pass
> `--storage-directory ~/.local/share/hisenseac/chip-tool` (or just use the
> widget / `hisense-ac provision`).

### Starting over / forgetting the AC

To un-pair and return to the Setup form, use the **Forget air conditioner**
button at the bottom of the popup (click it twice to confirm). The daemon asks
the device to drop the fabric, and only then wipes its local store:

```sh
hisense-ac unpair
```

The local fabric is removed **only after the AC confirms** the unpair — so the
device never ends up holding an orphaned fabric. If the AC is unreachable at
that moment, nothing is removed and the daemon retries automatically as soon as
the AC is reachable again (shown as a message in the widget). You only need a
factory reset of the AC itself (its Wi-Fi/reset button) if the unit is
permanently gone or was hard-reset.

## ConnectLife cloud backend (optional)

The cloud backend mirrors the official phone app: it logs in to the ConnectLife
(Higyu) service, polls the unit every 30 s, and writes properties on demand. It
needs **no Matter commissioning** – just your ConnectLife account. All modes
(including Dry and Fan-only), fan speed, swing, eco, super, sleep and fan-mute
are available through it.

1. Put your login in the config file (created by `scripts/install.sh`):

   ```sh
   $EDITOR ~/.config/hisenseac/connectlife.conf
   ```

   ```ini
   [connectlife]
   login=you@example.com
   password=
   # Optional. Pin a unit by its puid; empty uses the only AC in the account.
   deviceId=
   ```

2. Enable the daemon:

   ```sh
   systemctl --user enable --now hisense-connectlife
   journalctl --user -u hisense-connectlife -f   # watch it connect
   ```

The daemon registers `org.kde.hisenseac.connectlife` / `/HisenseAC` /
`org.kde.hisenseac.ConnectLife`. In the widget settings, pick the
backend (**Matter / ConnectLife cloud**) and use the switches to run only
that daemon (the other will be stopped and disabled).

The credentials are sent only to the ConnectLife/Higyu servers, exactly like the
phone app. Requests are signed with the app's embedded RSA public key and the
resulting access token is cached (mode `600`) for ~12 h so restarts do not
re-login (Gigya rate-limits repeated logins).

CLI options:

```
hisense-connectlife [--config <file>] [--login <email>] [--password <pw>]
                    [--device <puid>] [--interval <seconds>]
```

> This backend uses a reverse-engineered, unofficial API. It can break if Hisense
> changes the service, and repeated failed logins are rate-limited per IP.

## Running (Matter daemon)

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
hisense-ac provision "<ssid>" "<wifi-password>" "<pairing-code>"
                               commission the AC (needs it in pairing mode)
hisense-ac unpair              remove the pairing and forget local state
```

Then add the **Hisense AC** widget to your panel or desktop. Choose the
backend in settings. Selecting a backend starts and enables its daemon
and stops and disables the other (so the choice survives reboots). The
"Run the selected daemon" switch turns the selected one on or off.

On the Matter backend the widget distinguishes *commissioned* (paired — the
pairing persists in the fabric store) from *reachable* (answering right now).
Once paired, the controls stay available even while the AC is powered off; if the
unit drops off Wi-Fi the widget keeps showing the last known state with an
**offline** hint instead of asking you to pair again.

## Uninstall

```sh
scripts/uninstall.sh            # keeps the commissioned state
scripts/uninstall.sh --purge    # also deletes ~/.local/share/hisenseac
```

This stops/disables the daemons, removes the systemd user units, uninstalls the
plasmoid and deletes the installed binaries. Your ConnectLife config
(`~/.config/hisenseac/connectlife.conf`) is left in place. The Matter fabric is
kept by default so a re-install stays paired; `--purge` removes it as well (you
will need to re-run `provision.sh` afterwards).

## Known limitations

### Matter backend

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
  widget therefore offers only **Cool** and **Heat** over Matter. (Off is handled
  by the power button.)
- **Attestation is bypassed** at commissioning time
  (`--bypass-attestation-verifier 1`) because no PAA/CD trust store is shipped.
  Fine for a self-owned device on a trusted LAN; tighten for anything else.
- Temperatures are reported/received in **degrees Celsius**.
- Setpoint writes are re-applied after power-on (the unit can revert a setpoint
  set while off).
- Single AC / single fabric per store.

Use the **ConnectLife cloud backend** to work around the fan/swing/mode gaps
above: it drives the unit through the vendor API and can set every mode and
feature the app can.

### Cloud backend

- Requires your ConnectLife account credentials and an internet connection.
- Uses a **reverse-engineered, unofficial API**; Hisense can change or break it
  at any time. Login failures are rate-limited per IP.
- Polls every 30 s (configurable with `--interval`); state changes made outside
  the daemon appear at most one poll later.

## Repository layout

```
src/            C++ daemons + CLI client
                (ChipCli, Controller, main | ConnectLifeApi, ConnectLifeController,
                 connectlife_main, client)
plasmoid/       Plasma 6 plasmoid (org.kde.hisenseac, dual-backend)
scripts/        provision / import-current / install helpers
packaging/      systemd user units + connectlife.conf.example
docs/           architecture notes
```

See [`docs/architecture.md`](docs/architecture.md) for the D-Bus contracts and
implementation details.

## License

MIT.
