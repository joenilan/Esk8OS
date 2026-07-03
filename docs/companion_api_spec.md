# ESK8OS Companion App - BLE API Specification

This document details the custom Bluetooth Low Energy (BLE) protocol that the ESP32 firmware will expose. You can hand this document to any future agent or Android developer to instantly build a companion app that natively syncs with your longboard.

## 1. Overview
Instead of multiplexing the raw VESC UART protocol, the ESP32 acts as the single "master" querying the VESC. The ESP32 parses the data, runs the math (speed, range, battery limits), and broadcasts the final display-ready values over a custom BLE service using lightweight JSON. 

The Android app acts as a read/write client to this custom service.

## 2. BLE UUIDs

**Base Service UUID:** `5043697A-0000-4682-93CB-33BB0A149F7E`

| Characteristic | UUID | Properties | Purpose |
| :--- | :--- | :--- | :--- |
| **Telemetry** | `5043697A-0001-4682-93CB-33BB0A149F7E` | `NOTIFY` | 5 Hz core ride data (Speed, Battery, etc) |
| **Settings** | `5043697A-0002-4682-93CB-33BB0A149F7E` | `READ`, `WRITE` | Board configuration (Theme, Wheel size, MPH/KMH) |
| **Command** | `5043697A-0003-4682-93CB-33BB0A149F7E` | `WRITE` | Triggers for actions (Trip Reset, Change Page) |
| **Session** | `5043697A-0004-4682-93CB-33BB0A149F7E` | `NOTIFY` | 1 Hz trip/session statistics (fw 0.9.4+) |

---

## 3. Telemetry (Characteristics `0001` + `0004`)
Since fw 0.9.4 telemetry is split across two notify characteristics so neither
payload can approach the BLE notify size limit (a notify silently truncates past
ATT_MTU−3; the old merged payload measured ~503 bytes mid-ride):

- **`0001` core, 5 Hz (every 200 ms):** the fast-changing ride fields — `live`,
  `vesc`, `mph`, `spd`, `bat`, `v`, `w`, `mtr_t`, `esc_t`, `btemp`, `rng`,
  `bata`, `mota`, `duty`, `pkw`, `cellv`, `rwarn`, `fault`, `ppm`, `ppmok`,
  `slave`, `m1a`, `m2a`.
- **`0004` session, 1 Hz:** trip/session statistics — `max_s`, `wh`, `whr`,
  `mpw`, `minv`, `minvl`, `mba`, `mpa`, `sagc`, `thome`, `tlimp`, `avs`, `trip`,
  `odo`, `est`, `lrng`, `lest`, `eff`, `rtime`, `tmov`, `lfault`, `fw`.

The app subscribes to both, caches the latest session frame, and folds it under
each core frame (`{...session, ...core}`) so the rest of the app still sees one
merged telemetry object. Firmware older than 0.9.4 has no `0004` characteristic
and sends **all** fields on `0001` — the same merge handles that transparently.
The combined field reference below is unchanged.

All distance/speed/range/efficiency values are in the **board's configured display
unit** (mph + miles when `mph:true`, otherwise km/h + km), already converted by the
firmware — the app must **not** re-convert. Efficiency `eff` is Wh/mi (mph) or Wh/km.

When `live:false`, live values such as voltage, battery %, speed, watts, amps,
duty, temperatures, and remaining range must be treated as unavailable. This
happens when demo mode is off and no real VESC telemetry has arrived recently;
it is not an ESP32 supply-voltage reading.

**Example Payload:**
```json
{
  "live": true,      // Current live values are valid
  "vesc": true,      // Recent real VESC packet received
  "spd": 24.5,       // Current speed (board's display unit)
  "bat": 85,         // Battery percentage (0-100)
  "v": 45.2,         // Battery voltage
  "w": 650,          // Current power in Watts
  "mtr_t": 45,       // Motor temp (Celsius)
  "esc_t": 38,       // ESC temp (Celsius)
  "btemp": 30,       // Battery temp (Celsius)
  "rng": 12.4,       // Ride-home remaining range to homeCell floor (display unit)
  "max_s": 40.1,     // Session max speed (display unit)
  "wh": 120.4,       // Session Watt-Hours used
  "bata": 14.2,      // Battery current (A)
  "mota": 22.8,      // Motor current (A)
  "duty": 35,        // Motor duty cycle (%)
  "pkw": 1850,       // Live peak-hold power (W) — "peak now"
  "mpw": 2100,       // Session max power (W) — "max ride"
  "whr": 6.2,        // Session regen energy (Wh)
  "minv": 41.0,      // Session minimum voltage
  "minvl": 36.5,     // Lowest loaded/discharge voltage this session
  "mba": 37.9,       // Max battery current this session (A)
  "mpa": 61.4,       // Max motor current this session (A) — peak pull, fw 0.9.5+
  "cellv": 3.65,     // Loaded pack voltage per cell
  "rwarn": 2,        // Range warning: 0 ok, 1 turn-home, 2 voltage sag, 3 limp
  "sagc": 3,         // Count of loaded voltage dips below homeCell floor
  "thome": 18,       // Seconds below homeCell floor under load
  "tlimp": 0,        // Seconds near/under stopCell floor under load
  "avs": 18.3,       // Session average speed (display unit)
  "trip": 6.2,       // This-trip distance (display unit)
  "odo": 412.5,      // Lifetime odometer (display unit)
  "est": 15.7,       // Ride-home full-charge range to homeCell floor (display unit)
  "lrng": 19.7,      // Limp remaining range to stopCell floor (display unit)
  "lest": 21.6,      // Limp full-charge range to stopCell floor (display unit)
  "eff": 25.9,       // Avg efficiency — Wh/mi (mph) or Wh/km
  "fault": 0,        // VESC fault code (0 = none)
  "rtime": 1843,     // Board uptime since power-on this boot (seconds)
  "tmov": 1290,      // Trip moving-time — seconds spent rolling (>2 km/h), board-authoritative
  // --- remote input + diagnostics ---
  "ppm": 0.42,       // Decoded remote throttle, -1.0..+1.0 (<0 brake, >0 accel)
  "ppmok": true,     // Remote signal present (valid PPM pulse)
  "lfault": 0,       // Most recent VESC fault code, latched (0 = none seen)
  "slave": true,     // Second motor responding over CAN
  "m1a": 18.3,       // Master motor current (A)
  "m2a": 17.9,       // Slave motor current (A)
  "fw": "6.2"        // VESC firmware version (major.minor)
}
```

> **Remote (`ppm`/`ppmok`):** decoded from the master VESC via `COMM_GET_DECODED_PPM`
> — it's the VESC's decoded input (reflects PPM calibration), not the raw receiver
> channel. Throttle/brake position + signal-present only; **remote battery/buttons are
> NOT available over PPM** (one-way pulse train — would need the remote's UART telemetry
> wired to a VESC COMM port).

> **`tmov` vs `rtime`:** `tmov` is the canonical trip time the app should display — it
> accumulates only while the board is actually rolling, persists to NVS, and survives a
> power-cycle (the board continues the same trip on cold boot). It auto-resets after 6 h
> parked or on `TRIP_RESET`. `rtime` is raw board uptime this boot (parked time
> included) and is only useful as a system/diagnostics/session-log value.

---

## 4. Settings (Characteristic `0002`)
Allows the companion app to remotely configure the ESP32 without touching the physical buttons.

### Read
When the Android app connects, it reads this characteristic to get the board's current configuration.
```json
{
  "hw": "tdisplay-s3",
  "display": "tft",
  "ui": "full",
  "buttons": true,
  "mph": true,
  "theme": "cam",
  "poles": 14,
  "wheel": 105,
  "gear": 2.4,
  "bat_s": 12,
  "packAh": 16.5,
  "homeCell": 3.40,
  "stopCell": 3.30,
  "homeEff": 3.57,   // read-only, fw 0.9.5+: sag-lifted LOADED home floor the estimate actually stops at
  "stopEff": 3.47,   // read-only, fw 0.9.5+: sag-lifted LOADED limp floor (homeCell/stopCell are RESTING)
  "whmi": 25.9,
  "bright": 100,
  "demo": false,
  "rider": "JOE",
  "hud": "speed",
  "bfocus": "pct"
}
```

| Key | Type | Writable | Range / notes |
| :--- | :--- | :--- | :--- |
| `fwv` | string | ❌ | fw 0.9.5+: ESK8OS firmware version, e.g. `v0.9.5 9fc0918` (for the app's About page) |
| `hw` | string | ❌ | hardware target: `tdisplay-s3`, `esp32s3-oled`, `esp32s3-headless`, or generic `esp32s3` |
| `display` | string | ❌ | onboard display class: `tft`, `oled`, or `none` |
| `ui` | string | ❌ | onboard UI class: `full`, `mini`, or `headless` |
| `buttons` | bool | ❌ | true when the firmware target has local navigation buttons |
| `mph` | bool | ✅ | mph+mi vs km/h+km display unit |
| `theme` | string | ✅ | case-insensitive theme name |
| `bat_s` | int | ✅ | battery series cell count, 6–14 |
| `profile` | int | ✅ | wheel-preset index; **drives** read-only `poles`/`wheel`/`gear` |
| `poles` / `gear` | — | ❌ | read-only, derived from `profile` |
| `wheel` | int | ❌ | effective wheel diameter (mm) used for speed/distance — the preset's nominal, or `wheelmm` when set |
| `wheelmm` | int | ✅ | fw 0.9.5+: rider wheel-size calibration (mm), 120–350; `0` = use the preset. Overrides `wheel`; cleared when `profile` changes |
| `packAh` | float | ✅ | effective pack capacity (Ah), 4.0–40.0 |
| `homeCell` | float | ✅ | per-cell ride-home/range-planning floor, clamped to `stopCell`–4.20 |
| `stopCell` | float | ✅ | per-cell limp/dead cutoff floor, 3.00–3.60 |
| `whmi` | float | ✅ | range model default Wh/mile, 14.0–40.0 |
| `bright` | int | ✅ | display brightness %, 10–100 |
| `demo` | bool | ✅ | synthetic demo telemetry on/off |
| `rider` | string | ✅ | rider name shown in the header (≤15 chars), persisted to NVS |
| `hud` | string | ✅ | board HUD face: `speed`, `battery`, or `watts` |
| `bfocus` | string | ✅ | battery HUD hero value: `pct` or `volts` |
| `name` | string | ✅ | BLE advertised board name (scan list); reboot to re-advertise |
| `vtype` | int | ✅ | vehicle kind — see the table below; drives the app's vehicle icon/label |
| `vlabel` | string | ✅ | fw 0.9.5+: rider-typed name for a **custom** vehicle (`vtype` 5), ≤18 chars |
| `vicon` | int | ✅ | fw 0.9.5+: app icon index for a custom vehicle, 0–11 |
| `calR` | int | ❌ | fw 0.9.5+: learned pack internal resistance, mΩ |
| `calA` | float | ❌ | fw 0.9.5+: typical riding battery draw, A |
| `calWhmi` | float | ❌ | fw 0.9.5+: board-learned consumption Wh/mi; `0` until learned |
| `calWh` | int | ❌ | fw 0.9.5+: measured deliverable pack energy, Wh; `0` until learned |

> **Range authority (fw 0.9.5+):** when `calR` is present the board learns its
> own range model on-device (consumption, pack IR, deliverable energy). The app
> must not auto-push a learned `whmi` over it — explicit user-initiated writes
> remain fine.

**`vtype` values** (a wire contract — new kinds are appended, never reordered):
`0` skateboard · `1` e-bike · `2` scooter · `3` moped · `4` car · `5` custom ·
`6` EUC (electric unicycle) · `7` onewheel. For `5` (custom), the rider sets
`vlabel` (name) and `vicon` (icon). EUC and onewheel have no Material glyph, so
the app renders them as vector icons; pre-0.9.5 apps fall back to a generic icon.

### Write
The Android app writes a JSON object to this characteristic to change settings. The ESP32 parses it, saves to non-volatile storage (NVS), and immediately repaints the display. *You can send partial updates.*
```json
{
  "theme": "cyber",
  "mph": false
}
```

---

## 5. Commands (Characteristic `0003`)
Writing a raw ASCII string to this characteristic triggers immediate physical actions on the ESP32. This allows your phone to act as a remote control for the dashboard UI.

| Command String | Action |
| :--- | :--- |
| `"TRIP_RESET"` | Zeros the current trip — distance, moving-time (`tmov`), and session max metrics — and persists the cleared values. |
| `"PAGE_NEXT"` | Swipes the physical ESP32 display to the next page. |
| `"PAGE_PREV"` | Swipes the physical ESP32 display to the previous page. |
| `"PAGE_SET:<n>"` | Jumps the display to an **absolute** page index (0=HUD,1=DASH,2=POWER,3=TRIP,4=SETTINGS,5=SYSTEM,6=GRAPHS,7=LOGS). Use this for page-swipe sync when the app's page set doesn't 1:1 the board's. |
| `"BRIDGE_MODE"` | Requests VESC Tool Bridge Mode. On boards with buttons the display shows **START BRIDGE? L=YES R=NO** — the rider must physically confirm (30 s timeout) because the BLE link is unauthenticated and the bridge exposes motor config. Buttonless builds enter directly. |
| `"WIFI_EXPORT_START"`| Requests the ESP32's WiFi AP + HTTP server (logs + OTA) without entering VESC bridge mode — telemetry/BLE keep streaming. On boards with buttons the display shows **ALLOW WIFI? L=YES R=NO** (30 s timeout); the AP only rises after the rider confirms. |
| `"WIFI_EXPORT_STOP"` | Drops the standalone WiFi AP / HTTP server started by `WIFI_EXPORT_START`. |
| `"REBOOT"` | Reboots the ESP32 display. |

## 6. Hybrid WiFi Session-Log Transfer (Bulk Data)
The ESP32 writes one board session CSV from dashboard boot until power-off/reboot. It records at 1 Hz while the dashboard is running and is not reset by `TRIP_RESET` or phone trip recording. Because session logs can easily exceed 500 KB, attempting to download files this large over BLE is mathematically slow and prone to timeout errors.

To download historical logs, the Android app should use a **Hybrid Transfer**:
1. The app writes `"WIFI_EXPORT_START"` to the Command characteristic (`0003`).
2. The rider confirms **ALLOW WIFI?** on the board (L=YES); the ESP32 then spins up its `ESK8-BRIDGE` WiFi Access Point.
3. The Android app prompts the user to join the `ESK8-BRIDGE` network. The password is **per-device** (fw 0.9.4+): `esk8-` + the board's pair code — read it from the `wifiPass` settings field (§4) or the board's bridge screen. Firmware older than 0.9.4 uses the legacy fixed password `esk8bridge`.
4. The Android app hits the ESP32's internal HTTP Web Server (`http://192.168.4.1/`) to download the raw session CSV files over fast TCP.
5. Once downloaded, the user disconnects from the WiFi, and the board seamlessly resumes BLE telemetry.

## 7. Implementation Notes for Android Devs
- **MTU Negotiation:** Ensure the Android BLE client requests a high MTU (e.g., `512` bytes) upon connection. JSON payloads are small, but they will exceed the default `20` byte BLE limit. **The telemetry notify is sent as one JSON object** — without a negotiated MTU it will silently truncate.
- **Connection Loss:** If the Android app disconnects, the ESP32 will automatically resume advertising in the background.

## 8. Firmware Implementation Status (as built)
The ESP32 side of this spec is implemented in `src/services/companion_ble.cpp` (NimBLE + ArduinoJson). Notes on what is live and where reality differs from the draft above:

- **Always-on service.** The companion service initializes in `setup()` and advertises 100% of the time. The device name is **`ESK8-BLE`** (the companion service UUID is in the primary advertisement; the name + VESC-Tool NUS UUID are in the scan response). Use **active scanning** and filter by the service UUID `5043697A-0000-…`.
- **Co-existence with VESC Bridge mode.** The VESC-Tool NUS bridge shares the *same* NimBLE server. Entering Bridge mode (physically, or via the `BRIDGE_MODE` command) does not tear down the companion service — but **telemetry notifications pause** while bridging (consistent with §6), and queued Settings/Command writes are applied once the dashboard resumes.
- **Settings reads (§4):** `hw`, `display`, `ui`, and `buttons` are read-only capability fields so the app can adapt for full TFT, mini OLED, and headless firmware targets. `wifiSsid` / `wifiPass` (fw 0.9.4+) are the read-only log/OTA AP credentials — the password is per-device.
- **Settings writes (§4):** `mph` (bool), `theme` (string, case-insensitive), `bat_s` (int, 6–14), `profile` (int index), `packAh` (float, 4.0–40.0), `homeCell` (float, `stopCell`–4.20), `stopCell` (float, 3.00–3.60), `whmi` (float, 14.0–40.0), `bright` (int, 10–100), `demo` (bool), `hud` (`speed`/`battery`/`watts`), and `bfocus` (`pct`/`volts`) are honored and persisted to NVS; numeric fields are clamped to the listed ranges. `poles`, `wheel`, and `gear` are **read-only** — they are derived from the selected wheel preset, not independently settable. To change them, write **`profile`** (int index) to select a preset. `gear` is reported as the firmware's motor:wheel pulley ratio. Writing `homeCell`/`stopCell` recomputes home/limp range; `packAh`/`whmi` refresh the range estimate; `bright` applies to the backlight immediately.
- **`WIFI_EXPORT_START` / `WIFI_EXPORT_STOP` (§5/§6):** runs a *standalone* web service — it raises the `ESK8-BRIDGE` AP + `http://192.168.4.1/` (board session-log download **and** OTA firmware upload) **without** entering VESC Bridge mode, so the dashboard keeps running and BLE telemetry keeps streaming (true "hybrid" transfer). The same pages are also served while in VESC Bridge mode (over the bridge's own AP), so logs/OTA are reachable in either state — bridged or unbridged. The standalone AP auto-drops after 10 min idle. On-device, the same service can be toggled over USB serial with `wifi on` / `wifi off`.
- **Settings/Command writes** are processed on the firmware's UI thread (BLE callbacks only enqueue), so display repaints triggered by a write are race-free.
