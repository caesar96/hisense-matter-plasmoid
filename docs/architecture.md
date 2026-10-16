# Architecture

```
+---------------------------+
|  Plasma plasmoid (QML)    |   org.kde.hisenseac
|  org.kde.hisenseac        |
|                           |
|  Plasma5Support           |   runs on a timer (onNewData)
│  executable datasource ───┼──> hisense-ac status --json
|                           |
|  command runner           |
│  (connectSource)          |──► hisense-ac mode cool / setpoint 25 / on|off
+-------------+-------------+
              | one-shot subprocess
              v
        +-----------+      D-Bus (session bus)
        | hisense-ac |  <---------------------------+
        +-----------+                               |
                                                     |
        org.kde.hisenseac /HisenseAC                 |
        org.kde.hisenseac.Control                    |
                                                     v
                                          +---------------------+
                                          |  hisense-matterd    |
                                          |  Controller (QObject)|
                                          +----------+----------+
                                                     | ChipCli
                                                     v
                                          +---------------------+
                                          | chip-tool interactive|  (one warm
                                          |   start             |   process)
                                          +----------+----------+
                                                     | CASE over IPv6/UDP
                                                     v
                                              Hisense AC (Matter)
```

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

Methods:

```
Refresh()                       re-read all attributes from the device
SetPower(bool on)               on -> last non-off mode (default cool)
SetMode(string mode)            off|auto|cool|heat|fan|dry (only cool/heat
                                change the physical unit; see limitations)
SetSetpoint(double celsius)     writes cooling, or heating when in heat mode
SetCoolingSetpoint(double c)    write OccupiedCoolingSetpoint
SetHeatingSetpoint(double c)    write OccupiedHeatingSetpoint
```

`hisense-matterd` registers the `Controller` QObject directly with
`QDBusConnection::registerObject(..., ExportAllSlots | ExportAllProperties |
ExportAllSignals)`. Qt automatically implements `Properties.Get/Set/GetAll` and
emits `PropertiesChanged` when the shared `stateChanged()` NOTIFY signal fires.

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

## Limitations carried over from the device

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
