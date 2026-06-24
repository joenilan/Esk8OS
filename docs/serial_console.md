# ESK8OS Serial Console

A USB serial console for bench debugging and driving the board without a BLE
connection. Useful for both humans and agents — read live telemetry, inspect
config, and perform actions (toggle demo, reset trip, reboot, …).

## Connecting

- **Port / baud:** the board enumerates as a USB CDC serial device (e.g. `COM5`
  on Windows) at **115200 baud**.
- **Build requirement:** the console reads from `Serial`, which is only on the USB
  port in the **`tdisplay_s3_debug_usb`** / **`tdisplay_s3_touch_debug`** builds
  (`-DARDUINO_USB_CDC_ON_BOOT=1`). The `*_ride_release` builds drop USB CDC, so the
  console is unavailable there.
- Open with any serial terminal, or the helper scripts below. Commands are
  newline-terminated (`\r`, `\n`, or `\r\n`); arrow-key escape sequences are
  ignored so they don't corrupt input.

> Tip: set the terminal to **not** assert DTR/RTS on open — toggling them resets
> the ESP32. The helper scripts already do this.

## Helper scripts

| Script | Purpose |
| :--- | :--- |
| `scripts/serial_query.py [cmd ...]` | Send one or more console commands and print the replies. Defaults to `help odo free logs` if none given. |
| `scripts/serial_fetch.py` | Pull ride-log CSVs off the board over serial. |

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
| `stat`, `tel` | Live telemetry: speed, battery %, volts, VESC link/fault, power + peak, battery/motor amps, duty, motor/ESC/battery temps, energy used/regen. |
| `trip` | Trip distance, moving-time (`tmov`), odometer, session avg/max speed, range estimate (learned vs default). |
| `sys` | Firmware version, uptime, reset reason, FPS, free heap (+ min), free/total PSRAM. |
| `cfg`, `config` | Units, demo, brightness, theme, rider, battery (cells / pack Ah / stop V·cell / Wh-per-mi), active wheel profile. |
| `odo` | Odometer + trip + `tmov` (one line). |
| `logs`, `ls` | List ride-log files + storage use. |
| `cat <file>` | Dump a ride CSV (e.g. `cat r0001.csv`). |
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
| `rm <file\|all>` | Delete one ride file, or all of them. |
| `log [on\|off]` | Enable/disable ride logging. |
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
battery 10 cells | pack 16.5 Ah | stop 3.30 V/cell | 25 Wh/mi
wheel prof 0: 8IN PNEU (203 mm, 16/72, 7.0 pp)

>>> trip
trip 0.03 mi | tmov 00:00:08 | odo 16.38 mi
avg 0.0 mph | max 0.0 mph | range est 18.3 mi (rem 0.0) [default]
```

## Notes

- **Persistence:** `demo`, `units`, `bright`, and `rider` write to NVS immediately
  and survive a reboot. Trip distance + `tmov` persist on stop / every 60 s while
  riding (see the trip model), but **not** while demo mode is on (demo
  intentionally skips the odo/trip save).
- **No VESC connected:** `stat` will show idle/garbage values and a non-zero fault
  with `link DOWN` (or a stale `OK` right after boot) — that's just the absent ESC,
  not a console bug. Use `demo on` to exercise the telemetry pipeline on the bench.
