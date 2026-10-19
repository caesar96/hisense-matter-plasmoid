# Architecture

```
                    +-----------------------------------+
                    |        Plasma plasmoid (QML)      |
                    |        org.kde.hisenseac          |
                    |                                   |
                    |  org.kde.plasma.workspace.dbus     |
                    |  two DBus.Properties + two         |
                    |  DBusServiceWatcher (controlled by settings) |
                    |  the active backend, reads GetAll /
                    |  PropertiesChanged and calls methods
                    +--------+-----------------+--------+
                             |                 |
               D-Bus session bus               |
                             |                 |
       +---------------------+                 +-----------------------+
       |                                                                 |
       v org.kde.hisenseac.connectlife          org.kde.hisenseac       v
       | /HisenseAC                              /HisenseAC
       | org.kde.hisenseac.ConnectLife           org.kde.hisenseac.Control
       |                                                                 |
+-----------------------+                                  +------------------------+
| hisense-connectlife   |                                  | hisense-matterd        |
| ConnectLifeController |                                  | Controller (QObject)   |
+----------+------------+                                  +-----------+------------+
           | ConnectLifeApi                                            | ChipCli
           v                                                           v
  ConnectLife / Higyu cloud                             +---------------------------+
  (signed REST over HTTPS)                             | chip-tool interactive     |
                                                       | (one warm process)        |
                                                       +------------+-------------+
                                                                    | CASE over IPv6/UDP
                                                                    v
                                                            Hisense AC (Matter)

   +-------------+
   |  hisense-ac |  thin CLI client for the Matter daemon
   +------+------+  (D-Bus org.kde.hisenseac.Control, session bus)
          v
```

The plasmoid supports two backends (pinned in settings):

- **`backend` = `matter` or `cloud`** – talk to the selected daemon.
- Settings: `matterEnabled`, `cloudEnabled` – selecting a backend sets its flag
  and clears the other; the widget then calls systemd (user instance) to
  `StopUnit`/`DisableUnitFiles` the other and `EnableUnitFiles`/`StartUnit` the
  selected one. `SetUnitFileState` persistence (the enable symlink) makes the
  choice survive reboots. The single "Run the selected daemon" switch just
  turns the selected flag on/off.

When the selected backend changes or its flag flips, the policy is re-applied
and `updateAll()` is called on the new `DBus.Properties`.

## D-Bus interface

Service `org.kde.hisenseac`, object `/HisenseAC`, interface
`org.kde.hisenseac.Control`.

Properties (read via `org.freedesktop.DBus.Properties`):

| Property          | Type   | Meaning                                    |
|-------------------|--------|--------------------------------------------|
| `Connected`       | bool   | last read reached the device               |
| `Power`           | bool   | `SystemMode != 0`                          |
| `Mode`            | string | `off/auto/cool/heat/fan/dry/unknown`       |
| `SystemMode`      | uint   | raw Matter `Thermostat.SystemMode`         |
| `LocalTemperature`| double | °C                                         |
| `CoolingSetpoint` | double | °C                                         |
| `HeatingSetpoint` | double | °C                                         |
| `MinSetpoint`     | double | °C                                         |
| `MaxSetpoint`     | double | °C                                         |
| `NodeId`          | uint   | Matter node id (constant)                  |
| `LastError`       | string | last error, empty when healthy             |
| `Provisioning`    | bool   | a pairing run is in progress               |
| `ProvisionError`  | string | result of the last pairing run (`""` = ok) |
| `Unpairing`       | bool   | an unpair run is in progress               |
| `Commissioned`    | bool   | a fabric exists locally (independent of reachability) |

Methods:

```
Refresh()                       re-read all attributes from the device
SetPower(bool on)               on -> last non-off mode (default cool)
SetMode(string mode)            off|auto|cool|heat|fan|dry (only cool/heat
                                change the physical unit; see limitations)
SetSetpoint(double celsius)     writes cooling, or heating when in heat mode
SetCoolingSetpoint(double c)    write OccupiedCoolingSetpoint
SetHeatingSetpoint(double c)    write OccupiedHeatingSetpoint
Provision(ssid, pass, code)     commission the AC onto Wi-Fi / the fabric
Unpair()                        remove the fabric and wipe the local store
```

`Provision` is asynchronous: the daemon takes its warm `chip-tool interactive`
process offline (so the two never contend for the store), runs a one-shot
`chip-tool pairing code-wifi ... --bypass-attestation-verifier 1`, then restarts
the interactive process and refreshes. `Provisioning` flips true for the
duration and `ProvisionError` carries the outcome. While provisioning, reads and
writes are suppressed.

`Unpair` runs `chip-tool pairing unpair` the same way, but **only wipes the
local store if the device confirms** (otherwise the device would be left with an
orphaned fabric, requiring a factory reset). On failure it defers: `Unpairing`
clears, `ProvisionError` explains, and the next successful read (the device is
back) re-invokes `Unpair`.

`Connected` and `Commissioned` are deliberately separate. `Connected` means a
read succeeded just now; `Commissioned` (derived from the fabric keys in the
store) means we *can* talk to it. When the AC drops off Wi-Fi, `Connected` goes
false while `Commissioned` stays true, so the UI shows a stale-state hint rather
than the pairing form.

`hisense-matterd` registers the `Controller` QObject directly with
`QDBusConnection::registerObject(..., ExportAllSlots | ExportAllProperties |
ExportAllSignals)`. Qt implements `org.freedesktop.DBus.Properties.Get/Set/GetAll`
and `Introspectable`. The daemon also emits
`org.freedesktop.DBus.Properties.PropertiesChanged` whenever `stateChanged()` fires
so QML clients (via `org.kde.plasma.workspace.dbus.Properties`) update event-driven.

## Matter ⇄ friendly mapping

Matter reports temperatures in hundredths of a degree Celsius (2500 → 25.00 °C);
the daemon divides by 100 at the boundary.

`SystemMode` enum values used:

| value | mode |
|-------|------|
| 0     | off  |
| 1     | auto |
| 3     | cool |
| 4     | heat |
| 7     | fan (FanOnly) |
| 8     | dry  |

## chip-tool driving

`ChipCli` starts `chip-tool interactive start --storage-directory <store>` once
and writes newline-terminated commands to its stdin. It parses the coloured
`[TOO]` log lines after stripping ANSI escapes:

- reads match `<AttributeLabel>:\s*(-?\d+)`, e.g. `LocalTemperature: 2700`
- writes match `status = 0x.. (SUCCESS|FAILURE...)`

Commands are serialized (one in flight, 12 s timeout). A `Run command failure`
line fails the current job. If the process dies it is restarted before failing
the queue.

Two quirks discovered while building this:

1. **Global chip-tool options must come last**, e.g.
   `chip-tool thermostat read system-mode 1 1 --storage-directory DIR`.
2. **Setpoint-then-power ordering**: writing a setpoint while the unit is off can
   be reverted when it powers on, so mode/power is applied first and the state is
   re-read afterwards (`Refresh()` is scheduled ~300 ms after each write).

## Persistent storage

`--storage-directory DIR` relocates the commissioner state that otherwise lives
in volatile `/tmp`. The fabric is split across **two** files and both are
required, or chip-tool silently creates a second fabric that the AC rejects:

- `DIR/chip_tool_config.ini` – fabric table (`f/1/...`)
- `DIR/chip_tool_config.alpha.ini` – commissioner CA/ICA keys (`ExampleOpCreds*`)
- `DIR/chip_tool_kvs` – CHIP KVS (empty in practice)
- `DIR/chip_config.ini`, `chip_factory.ini`, `chip_counters.ini` – non-fabric config

`scripts/provision.sh` commissions straight into the store;
`scripts/import-current.sh` copies an existing `/tmp` commissioning into it.

> A bare `chip-tool` invocation (no `--storage-directory`) writes to `/tmp`,
> which is wiped on reboot and is a *different* fabric from the daemon's. Any
> manual `chip-tool` command must therefore pass
> `--storage-directory ~/.local/share/hisenseac/chip-tool`, or the daemon can no
> longer see the commissioned device.

## ConnectLife cloud backend

`hisense-connectlife` registers `org.kde.hisenseac.connectlife`, object
`/HisenseAC`, interface `org.kde.hisenseac.ConnectLife`, exporting the
`ConnectLifeController` QObject the same way (`ExportAllSlots |
ExportAllProperties | ExportAllSignals` + a manual `PropertiesChanged` on
`stateChanged`).

Properties:

| Property          | Type   | Meaning                                            |
|-------------------|--------|----------------------------------------------------|
| `Connected`       | bool   | the cloud reports the device online                |
| `Power`           | bool   | `t_power == 1`                                     |
| `Mode`            | string | `off/fan/heat/cool/dry/auto`                       |
| `Temperature`     | double | target setpoint, **°C** (always)                   |
| `LocalTemperature`| double | room temperature, °C                               |
| `FanSpeed`        | string | `auto/low/medium/high`                             |
| `Swing`           | bool   | swing/oscillation on: `t_up_down`, or direction/angle on split units |
| `Dimmer`          | bool   | display light on: `t_dimmer` (when reported)       |
| `Eco`             | bool   | eco toggle                                         |
| `Super`           | bool   | "super"/turbo toggle                               |
| `FanMute`         | bool   | fan-mute (quiet) toggle                            |
| `Sleep`           | bool   | sleep toggle                                       |
| `Humidity`        | double | room humidity, % (when reported)                   |
| `LastError`       | string | last error, empty when healthy                     |
| `RefreshingLogin` | bool   | a forced re-login is in progress                   |
| `DeviceId`        | string | puid of the controlled unit (constant)             |

Methods:

```
Refresh()                       re-read the device status now
RefreshLogin()                  drop the cached access token and log in again
SetPower(bool on)               on -> last non-off mode (default cool)
SetMode(string mode)            off|fan|heat|cool|dry|auto (off = power off)
SetTemperature(double celsius)  setpoint; device display unit is preserved
SetFanSpeed(string speed)       auto|low|medium|high
SetSwing(bool on)               portable units write t_up_down; split units t_swing_direction/angle
SetDimmer(bool on)              display light (`t_dimmer`)
SetEco(bool)  SetSuper(bool)  SetFanMute(bool)  SetSleep(bool)
```

Writes are optimistic: the controller updates its cached state immediately,
broadcasts `stateChanged`, and schedules a re-read (`Refresh`) ~2.5 s later to
reconcile with the device. A rejected write (the gateway returns a non-zero
`errorCode`) surfaces through `LastError`.

Mode codes are `0 fan, 1 heat, 2 cool, 3 dry, 4 auto`; fan codes are
`0 auto, 5 low, 7 medium, 9 high`. The four exposed speeds map onto the
`5/7/9` ladder, which the portable units (type `006`/`008`) accept; the
five-speed wall splits (type `009`) also accept these values, so no
per-device table is needed (codes `6`/`8`, and `2`/`3`/`4` seen on some
firmwares, are folded onto low/medium/high when read back). Temperatures are
always exposed in Celsius; on write the daemon converts to the unit the device is
currently configured for (`t_temp_type`: `0` = Celsius, `1` = Fahrenheit) so the
panel's °F display is not clobbered.

### Talking to the cloud

`ConnectLifeApi` reproduces the official app's request pipeline:

1. **Auth** – `accounts.login` on Gigya EU (login + password + app key) →
   `accounts.getJWT` → `oauth/authorize` → `oauth/token`, yielding a bearer
   access token. The token is cached under
   `~/.cache/hisenseac/connectlife-token.json` (mode `600`, lifetime from the
   token endpoint, ~12 h fallback) so restarts do not re-login; Gigya
   rate-limits repeated logins per IP, so logins are single-flight: concurrent
   requests share the one login chain already in progress.
2. **Read** – signed `GET /clife-svc/pu/get_device_status_list`; the daemon picks
   the unit matching the configured puid (or the only AC).
3. **Write** – signed `POST /device/pu/property/set` with `t_power`,
   `t_work_mode`, `t_temp`, `t_fan_speed`, etc.

Every request carries a set of common fields (`appId`, `appSecret`, `languageId`,
`randStr`, `timeStamp`, `timezone`, `version`) plus a signature: the fields are
`sorted → k=v&... → + salt → sha256 → RSA-encrypt (PKCS#1 v1.5) with the app's
public key → base64`. The public key is embedded in `src/connectlifeapi.cpp`
(verbatim copy of the vendor `pubkey.pem`). The signature contains base64 `+`,
so the request body is hand-built percent-encoded — `QUrlQuery` would leave `+`
literal and the gateway would form-decode it to a space, breaking the signature
(`101001 Signature check fail`). The write reply is checked on `errorCode` (not
`code`).

The daemon polls on a timer (default 300 s, `--interval`) and otherwise only polls
on demand after a write; there is no push/subscription. The gateway throttles
aggressive clients ("Api rate limit exceeded"), so the daemon defends itself:
status reads are single-flight (overlapping refreshes share one request), writes
are debounced and merged (800 ms, so a temperature-arc drag becomes a single
property set instead of one request per step), and a rate-limited reply (HTTP 429
or a rate-limit message in the body, from either the gateway or Gigya) puts the
API into an exponential backoff (60 s doubling per hit, capped at 30 min) during
which requests fail fast through `LastError` instead of being sent.

A cached token that the gateway no longer accepts produces that same
"Api rate limit exceeded" reply. `RefreshLogin()` (the widget's **Refresh login**
button) is the escape hatch: it drops the in-memory and on-disk token, resets
the backoff so the attempt is not swallowed, and runs the login chain again
(single-flight, so a concurrent request joins it instead of starting a second
Gigya login). The controller exposes `RefreshingLogin` while it runs and
re-reads the status once the login succeeds.

## Limitations (Matter backend) carried over from the device

The AC only implements the Thermostat cluster (plus Identify/Groups/Descriptor and
ThermostatUserInterfaceConfiguration). Its Thermostat cluster has just the 19
standard attributes (`FeatureMap = 3`, Heat+Cool). There is **no FanControl
cluster and no FanMode**, so fan speed cannot be set over Matter; likewise there
is **no swing/oscillation attribute and no display-light attribute**, so those
features are only reachable via the Hisense/ConnectLife app or the IR remote.

Because the FeatureMap advertises only Heat+Cool, the firmware only actually
implements two modes. Writing other `SystemMode` values behaves inconsistently:
`Dry` (8) is rejected with a device `FAILURE`, while `Auto` (1) and `FanOnly`
(7) are **accepted and even read back** (the device reports the new value) but
the physical unit is left in its previous mode — the write never reaches the
actuator. Only `Cool` (3) and `Heat` (4) change the hardware, so the plasmoid
offers just those.

Attestation verification is bypassed (no PAA/CD trust store installed).

None of these Matter limitations apply to the **ConnectLife cloud backend**,
which drives the vendor API and therefore reaches every mode and feature the
phone app can (fan speed, swing, dry, eco, ...). It trades the fully-local Matter
path for a dependence on the vendor cloud and the operator's ConnectLife
account.
