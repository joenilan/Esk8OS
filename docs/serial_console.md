# ESK8OS Serial Console

A USB serial console for bench debugging and driving the board without a BLE
connection. Useful for both humans and agents — read live telemetry, inspect
config, and perform actions (toggle demo, reset trip, reboot, …).

## Connecting

- **Port / baud:** 115200 baud. LilyGO debug builds enumerate as USB CDC
  serial. Generic ESP32-S3 builds also mirror the console on `Serial0`, so the
  CH343/USB-UART port works too.
- **Build requirement:** the console is available in `tdisplay_s3_debug_usb`,
  `tdisplay_s3_touch_debug`, `esp32s3_headless_usb`, and
  `esp32s3_oled_i2c_usb`. LilyGO ride-release builds drop USB CDC, so the
  console is unavailable there.
- Open with any serial terminal, or the helper scripts below. Commands are
  newline-terminated (`\r`, `\n`, or `\r\n`); arrow-key escape sequences are
  ignored so they don't corrupt input.

> Tip: set the terminal to **not** assert DTR/RTS on open — toggling them resets
> the ESP32. The helper scripts already do this.

## Silent Serial Troubleshooting

If the LilyGO appears as `USB Serial Device (COMx)` but does not answer commands,
do not immediately assume the ESP32 is dead.

Known failure mode fixed in firmware: when `demo` was persisted `OFF` and the
VESC UART was disconnected/not responding, `waitForBootReady()` used to block
forever waiting for `UART.getVescValues()`. The serial console only starts after
`setup()` finishes, so `sys`/`cfg` appeared dead even though USB and flashing
still worked. Current firmware times out the VESC boot wait after about 3.5 s,
shows `NO VESC - BLE READY`, and continues to the console.

Recovery checklist:

1. Confirm Windows sees a COM port:
   ```powershell
   [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
   ```
2. Confirm the USB path is alive by flashing the debug build:
   ```powershell
   pio run -e tdisplay_s3_debug_usb -t upload --upload-port COM5
   ```
3. Wait about 6 s after reset, then query:
   ```powershell
   python scripts\serial_query.py sys cfg
   ```
4. If opening the port with RTS asserted prints an `ESP-ROM` reset banner, USB
   reset and ROM serial output are alive; focus on firmware boot flow or VESC
   UART wiring.
5. Use `demo on` for bench work when the VESC UART is not connected.

Use `tdisplay_s3_debug_usb` for serial work. The `tdisplay_s3_ride_release`
environment intentionally disables USB CDC, so it is not appropriate for Claude
or Codex serial debugging.

## Helper scripts

| Script | Purpose |
| :--- | :--- |
| `scripts/serial_query.py [cmd ...]` | Send one or more console commands and print the replies. Defaults to `help odo free logs` if none given. |
| `scripts/serial_fetch.py` | Pull session-log CSVs off the board over serial. |

Both default to `COM5` — edit the `PORT` constant if your board enumerates
elsewhere.

```bash
python scripts/serial_query.py stat trip sys
python scripts/serial_query.py "demo on"
```

## Commands

Type `help` (or `?`) for the in-firmware list. `[...]` = optional argument.

### Status / debug (read-only)

| Command | Output |
| :--- | :--- |
| `help`, `?` | List all commands. |
| `stat`, `tel` | Live telemetry: speed, battery %, volts, VESC link/fault, power + peak, battery/motor amps, duty, motor/ESC/battery temps, energy used/regen, loaded-cell sag evidence. |
| `diag` | Remote/PPM throttle (−1…+1, accel/brake) + signal-present, VESC firmware version, slave-motor (CAN) online, current/last fault, per-motor current + temps. |
| `trip` | Trip distance, moving-time (`tmov`), odometer, session avg/max speed, range estimate (learned vs default). |
| `sys` | Firmware version, uptime, reset reason, FPS, free heap (+ min), free/total PSRAM. |
| `cfg`, `config` | Units, demo, brightness, theme, rider, battery (cells / pack Ah / stop V·cell / Wh-per-mi), active wheel profile. |
| `odo` | Odometer + trip + `tmov` (one line). |
| `logs`, `ls` | List board session-log files + storage use. |
| `cat <file>` | Dump a session CSV (e.g. `cat s0001.csv`). |
| `free` | Filesystem partition usage. |
| `log` | Logging on/off status + free space. |
| `wifi` | Standalone log/OTA web-service status. |

### Actions

> **Destructive commands require confirmation.** `rm`, `odo reset`, `odo set`,
> `trip reset`, and `reboot` print a `… -- confirm? [y/N]` prompt and do nothing
> until you reply `y`/`yes` on the next line; anything else cancels.

| Command | Effect |
| :--- | :--- |
| `demo [on\|off]` | Toggle simulated telemetry (persisted). No arg = show state. |
| `units [mph\|kmh]` | Display units (persisted). No arg = show state. |
| `bright <10-100>` | Backlight % (persisted, applied immediately). |
| `rider [name]` | Rider name, ≤15 chars (persisted). No arg = show name. |
| `trip reset` | Zero the trip — distance, moving-time, and session max metrics — same as a BLE `TRIP_RESET` / left long-press. |
| `odo reset` | Zero the lifetime odometer. |
| `odo set <v>` | Set the odometer to `<v>` (in the active display unit). |
| `rm <file\|all>` | Delete one session file, or all of them. |
| `log [on\|off]` | Enable/disable board session logging. |
| `wifi [on\|off]` | Start/stop the standalone log/OTA web service (`http://192.168.4.1`). |
| `reboot` | Restart the board. |

## Example session

```
>>> sys
fw v0.9.3 e728018-dirty
uptime 00:00:05 | reset-reason 0 | fps 125
heap 131528 B free (min 131432) | psram 8385851/8386231 B free

>>> cfg
units mph | demo OFF | bright 100% | theme 0 | rider ZOMBIE
battery 10 cells | pack 16.5 Ah | home 3.40 V/cell | limp 3.10 V/cell | 25.9 Wh/mi
wheel prof 0: 8IN PNEU (203 mm, 16/72, 7.0 pp)

>>> trip
trip 0.03 mi | tmov 00:00:08 | odo 16.38 mi
avg 0.0 mph | max 0.0 mph | home 15.7 mi (rem 14.2) | limp 21.6 mi (rem 19.6) [default]
```

## Notes

- **Persistence:** `demo`, `units`, `bright`, and `rider` write to NVS immediately
  and survive a reboot. Trip distance + `tmov` persist on stop / every 60 s while
  riding (see the trip model), but **not** while demo mode is on (demo
  intentionally skips the odo/trip save).
- **No VESC connected:** live telemetry is marked unavailable and masked instead
  of showing stale pack voltage or current. Use `demo on` to exercise the
  telemetry pipeline on the bench.

## v0.10.2 additions — scripting, AI tooling, ESC passthrough

- `ping` — liveness + firmware version + uptime, for scripted handshakes.
- `json` — the full state (live telemetry + settings + VESC base config with
  per-value provenance) as one machine-readable JSON line. Parse this instead
  of scraping the human-readable commands.
- `set <key> <value>` — write any app-writable setting (companion spec §4
  keys: `mph`, `theme`, `bat_s`, `packAh`, `homeCell`, `stopCell`, `whmi`,
  `wheelmm`, `hud`, `rider`, `name`, …). Rides the SAME code path as the BLE
  settings write, so validation and persistence are identical.
- `unset <key>` — delete a rider override (`cells|packAh|stopCell|homeCell|
  whmi|wheelmm`); the value falls back to the VESC-read base, or the generic
  default if no ESC has ever been read.
- `vesc <cmd>` — passthrough to the ESC's own terminal (`vesc faults`,
  `vesc ping`, `vesc hw_status`, …). Needs a live/awake VESC; never collides
  with telemetry (serviced by the poll task) and is unavailable while bridge
  mode owns the UART.
- `faults` — shortcut for `vesc faults`.

The firmware also snapshots `vesc faults` **automatically** into the session
CSV (as `#`-prefixed comment lines, flushed immediately) whenever a fresh
fault code appears or the pack-disconnect tripwire fires — the ESC's fault log
is RAM-only and vanishes at its next power-up, so it must be captured in the
moment.
