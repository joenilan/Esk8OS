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
| **BaseConfig** | `5043697A-0005-4682-93CB-33BB0A149F7E` | `READ` | VESC-read base config + per-value provenance (fw 0.10.1+) |
| **BMS** | `5043697A-0006-4682-93CB-33BB0A149F7E` | `NOTIFY` | 1 Hz Daly BMS pack + per-cell (BMS builds only) |

---

## 2b. Base config (Characteristic `0005`, fw 0.10.1+)

The firmware's config is three-tiered: **rider override** (explicit NVS key) →
**VESC base** (parsed from the ESC's own `COMM_GET_MCCONF`, signature-gated) →
**neutral generic default**. This read-only characteristic exposes the base tier
and where each effective settings value currently comes from, so the app can
label overrides, show the ESC's ground truth next to them, and offer
"reset to base" semantics (deleting the NVS key = writing the settings field is
NOT that; a delete mechanism is future work — today the app just displays).

```json
{"valid":true,"cells":10,"ah":16.5,"cutS":34.0,"cutE":31.0,"poles":14,
 "gear":4.5,"wheel":203,"motA":57.0,"batA":15.0,"regA":-5.0,
 "src":{"cells":"r","ah":"v","home":"v","stop":"r","whmi":"r","wheel":"v"}}
```

- `valid:false` → no firmware-matched mcconf has ever parsed on this board;
  only `src` is present (everything will be `r` or `d`).
- `cutS`/`cutE`: the VESC's battery-cut start/end, **pack volts** (it ramps
  current between them). `gear` is wheel:motor (e.g. 72/16 = 4.5). `wheel` mm.
- `src` values: `r` = rider override, `v` = VESC base, `d` = generic default.
  Keys map to the settings fields `bat_s`/`packAh`/`homeCell`/`stopCell`/
  `whmi`/`wheel`. Older firmware lacks the characteristic entirely — treat as
  `valid:false`.

## 2c. BMS (Characteristic `0006`, BMS builds only)

The Daly smart BMS, re-exposed. ESK8OS is a BLE central to the installed Daly
"UART K" wireless module while remaining a BLE peripheral/server to the
companion phone, so the app gets **per-cell voltages** — the one thing the VESC
can't provide, and the whole reason to read the BMS: a single lagging cell is how
a DIY pack strands you or catches fire, and it's invisible from the pack
terminals. The dongle stays installed; no GPIO 43/44 BMS wiring is used.

Present only on firmware built with `-DESK8OS_BMS_DALY`. **Absent = the board has
no BMS integration** — the app should hide its pack view, exactly like any other
capability-gated surface. Notifies at **1 Hz** (cell voltages move slowly). When
no Daly has answered yet, `link:false` and no other fields are sent.

```json
{
  "link": true,        // BMS answered within the staleness window
  "fv": 511,           // fresh response groups, bits 0..8 (0x1ff = all fresh)
  "al": 1,             // alarm level: 0 none, 1 verified warning, 2 alert, 3 stale
  "pv": 39.62,         // pack voltage (V)
  "cur": -12.4,        // pack current (A): + = charging, - = discharging
  "soc": 78,           // state of charge (%)
  "rah": 7.84,         // remaining capacity (Ah)
  "cyc": 42,           // charge cycles
  "n": 10,             // cell count (pack topology, from the BMS)
  "cv": [3962,3968,3961,3970,3966,3959,3892,3964,3967,3960],  // per-cell mV, index 0 = cell 1
  "bal": 0,            // balancing bitmask: bit c set = cell (c+1) actively bleeding
  "st": 0,             // STALE bitmask: bit c set = cell (c+1)'s mV is old (dropped frame) — show "?"
  "mn": 3892, "mnc": 7,// lowest cell mV + its 1-based number
  "mx": 3970, "mxc": 4,// highest cell mV + its 1-based number
  "dv": 78,            // pack imbalance = max - min (mV); the number to watch
  "t": [26,28],        // temperature sensors (°C)
  "tmn": 26, "tmx": 28,// min/max temperature (°C)
  "cmos": true,        // charge MOSFET engaged
  "dmos": true,        // discharge MOSFET engaged
  "flt": false,        // any raw BMS protection/alarm flag active
  "fb": "00000000000000",// current seven raw 0x98 bytes, lowercase hex
  "lfb": "00000800000000",// optional: latest non-zero fault this board boot
  "lfa": 42,            // optional: seconds since lfb was observed
  "oc": [36.0,45.0,1000,60.0,500], // optional; verified pack settings
  "oca": 8              // optional: seconds since oc was read
}
```

- **Response freshness (`fv`):** a valid pack reply does not prove every later
  request in the same poll succeeded. Bits map to pack, min/max voltage,
  min/max temperature, MOS/remaining Ah, status, cells, temperatures, balance,
  and faults. Consumers must not treat a group whose bit is clear as current.
- **Alarm level (`al`):** `1` is reserved for the two Level-1 high-voltage
  warning bits verified on this physical pack. Discharge-overcurrent warning
  (`fb` byte 2 bit 2) and protection 1 (byte 2 bit 3) are both `2`, as is any
  still-unclassified bit; `3` means the fault response is stale. `fb` preserves
  the current raw evidence. Optional `lfb`/`lfa` latch the latest non-zero frame
  for the rest of the board boot so a self-clearing cutoff remains identifiable.
- **Discharge-overcurrent settings (`oc`):** optional five-element array:
  `[warning_A, protection1_A, protection1_delay_ms, protection2_A,
  protection2_delay_ms]`. It is a CRC-validated, read-only Modbus function-03
  result from official DALY registers `0x145..0x149`; `oca` is its age in
  seconds. Absence means the read has not succeeded. Firmware exposes no BMS
  register-write path, and these values are diagnostics rather than control
  inputs.
- **Per-cell staleness (`st`):** each cell's mV is stamped when its frame
  arrives; if a `0x95` frame drops, that cell keeps its old value but its bit in
  `st` is set. The app must render a stale cell as unknown ("?"), never as a
  live reading — a masked stale value defeats the entire point of per-cell data.
- **`cv` order:** index 0 is cell 1; `mnc`/`mxc` are **1-based** cell numbers to
  match how the pack is labeled physically.
- **`dv` (delta):** the single best imbalance indicator. A healthy pack sits in
  the low tens of mV under load; a delta that climbs with load points at a weak
  cell well before the pack-level voltage shows anything.
- **`cur` sign:** matches the Daly convention — positive charging, negative
  discharging. The 2026-07-30 joint ride verified sign and scale against VESC
  `bata`: integrated Daly discharge was 11.64 Ah and its remaining-capacity
  counter fell 11.81 Ah (about 1.4% difference). VESC current remains the
  drivetrain/safety/energy authority while live; normalized Daly current is
  available to the battery view only when VESC telemetry is absent.
- **`soc` / `rah`:** Daly coulomb-counter values. The same ride moved from
  `32.00 Ah / 100%` to `20.19 Ah / 63%`, consistent with the current integral.
  A fresh, in-range Daly `soc` is therefore the rider-facing `0001.bat` source.
  Voltage-derived SOC remains a provisional fallback if the counter is absent
  or invalid.

### Verified Daly "UART K" BLE transport (physical unit, 2026-07-28)

The installed unit was inspected over the air rather than identified from a
look-alike product listing:

- Advertised/GAP name: **`ZOMBIESK8-BMS`**
- Static-random address: **`D0:18:04:01:39:98`** (NimBLE address type `1`;
  the two high bits of `D0` are the static-random marker)
- Advertising manufacturer company ID/data: `0x0102 / 16d01804013998`
- The advertisement did not include a service UUID. Discovery therefore cannot
  filter on `FFF0`; this firmware matches the verified static-random address
  from a live scan and connects using the address type reported on-air.
- A Windows central connected and enumerated/read GATT without requesting a
  passkey, bonding, or explicit pairing.

The complete non-standard GATT table observed on this unit was:

| Service | Characteristic | Properties | Observed purpose |
| :--- | :--- | :--- | :--- |
| `0000FFF0-0000-1000-8000-00805F9B34FB` | `FFF1` | `READ`, `NOTIFY` | Daly byte TX, descriptor text `spss_tx_desc` |
| `0000FFF0-0000-1000-8000-00805F9B34FB` | `FFF2` | `READ`, `WRITE`, `WRITE_NR` | Daly byte RX, descriptor text `spss_rx_desc` |
| `0000FFF0-0000-1000-8000-00805F9B34FB` | `FFF3` | `READ`, `WRITE`, `WRITE_NR` | Present but unused by ESK8OS |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF00` | `READ` | Present; unused |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF01` | `WRITE`, `WRITE_NR` | Present; unused |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF02` | `READ`, `NOTIFY` | Present; unused |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF03` | `READ` | Present; unused |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF04` | `READ`, `WRITE`, `WRITE_NR`, `NOTIFY` | Present; unused |
| `02F00000-0000-0000-0000-00000000FE00` | `...FF05` | `READ`, `WRITE`, `WRITE_NR` | Present; unused |

Standard GAP `1800` and GATT `1801` services were also present. The data path
was verified directly: subscribing to `FFF1`, then writing the existing
`0x90` request `a540900800000000000000007d` to `FFF2` without a prior
application handshake produced the checksum-valid 13-byte notification
`a501900801a20000753003e871`. ESK8OS does not use or probe the unrelated
`02F0...` service. All existing read requests `0x90` through `0x98` then
produced checksum-valid Daly replies. The four `0x95` cell frames arrived
coalesced into one 52-byte notification, so the firmware deliberately treats
notifications as a byte stream rather than assuming one ATT notification per
Daly frame.

Although `FFF2` advertises both `WRITE` and `WRITE_NR`, this physical firmware
answered the legacy Daly requests only when they were sent as write-without-
response. ESK8OS therefore uses that verified form with a 100 ms inter-command
gap and does not infer success merely from a GATT connection. Reconnect backoff
is reset only after checksum-valid Daly telemetry arrives.

During bench stress testing the UART bridge once remained connectable but
returned short `FE/FF F8...` packets for both the legacy `0x90` request and a
captured read-only register request, even from a Windows central while the
ESP32 was held in its bootloader. Using **Restart system** in the official Daly
app and power-cycling the skateboard restored normal checksum-valid replies.
ESK8OS does not send that restart command automatically; it records the BMS link
as down and backs off instead.

The module continued advertising before, during, and after the Windows GATT
connection. A separate phone+Windows test then established the actual connection
limit: while the Daly Android app was connected and still receiving live data,
the module kept advertising but the Windows central timed out trying to connect.
Immediately after force-stopping the Daly app, the Windows central connected
successfully. This physical UART K module therefore accepts **one central at a
time**. Fully disconnect/force-stop the Daly app before expecting ESK8OS to own
the BMS link.

This limit applies to the UART K module, not to the ESP32-S3 radio as a whole.
ESK8OS remains a BLE central to UART K and a BLE peripheral/server to the
companion phone. Its failed-link reconnects back off from 2 s up to 60 s and do
not stop/restart companion advertising or disconnect an already-connected
companion phone.

### Captured Daly app protocol and settings behavior

An Android Bluetooth HCI capture of Daly app `V3.3.0.3` confirmed that the app
also uses `FFF2` writes and `FFF1` notifications, but with a newer,
Modbus-like register protocol alongside the legacy `0x90` telemetry protocol:

- Request address is `0x81`; reply address is `0x51`.
- Frames use Modbus CRC16, least-significant CRC byte first.
- After enabling `FFF1` notifications, the app writes
  `81 06 01 74 00 A2` before its read cycle, then repeatedly reads register
  blocks `0000..003F`, `0041..007E`, `0100..0150`, `0151..0177`, and
  `0178..01C1`. The exact semantic name of register `0x0174` is not proven, so
  it should be treated only as the observed app poll trigger/keepalive.
- On connection the app automatically writes the phone time to
  `0x0123..0x0125`. No user setting was changed during the capture.
- Parameter writes use function `0x06` for one register and function `0x10` for
  contiguous registers. This firmware intentionally does **not** expose BMS
  writes yet; a safe implementation needs confirmation, range checks, dependent
  register updates, response validation, and read-back verification.

The six-digit settings password is stored as ASCII in registers
`0x0126..0x0128`. The app compares the entered password locally; entering it
caused no BLE authentication/unlock command. It is therefore a UI safety gate,
not BLE pairing or cryptographic authorization.

The capture also explains the two alarms seen on this pack. Relevant values were:

| Meaning | Level 1 | Level 2/protection | Recovery | Delay |
| :--- | ---: | ---: | ---: | ---: |
| Cell high | 4.150 V (`0x0130`) | 4.250 V (`0x0131`) | 4.150 V | 1000 ms |
| Pack high | 41.5 V (`0x0138`) | 42.5 V (`0x0139`) | 41.5 V | 1000 ms |

The normal settings page labels/displays the Level-2 values, while the home page
reports the lower Level-1 warnings. At 41.8 V and roughly 4.18–4.19 V/cell,
`0x98` therefore correctly reports cell-high Level 1 and pack-high Level 1
without opening either MOSFET. The observed 10 mV cell spread is not evidence of
a weak or mismatched cell.

The 2026-07-30 ride captured a different event. DALY's official K/M/S BMSTool
V1.14.60 maps `0x98` byte 2 bit 2 to **discharge overcurrent alarm** and bit 3
to **discharge overcurrent protection 1**. The log changed from
`00000400000000` while drawing about 44 A to `00000800000000`; the discharge
MOS then opened for about 10 seconds. During that interval the Daly pack stayed
near 40 V, cell groups stayed near 4.03 V, maximum temperature was 32 °C, and
the VESC fault code remained zero while its input bus collapsed. This identifies
the interruption as a Daly discharge-overcurrent protection trip, not an empty
pack, weak-cell cutoff, temperature cutoff, or VESC fault.

Firmware now retrieves the exact configuration without the DALY phone app by
sending only Modbus function `0x03` (read holding registers) to `0x145..0x149`.
The verified request address is `0x81`; this UART K/BMS combination replies as
`0x51`. DALY encodes the three current thresholds as `(A × 10) + 30000`; the two
delays are milliseconds. Every response must have address `0x51`, function
`0x03`, byte count `10`, and a valid Modbus CRC before publication. A failure
does not interrupt legacy telemetry and retries no faster than every 30 seconds.
There is deliberately no function `0x06`/`0x10` settings-write implementation,
so monitoring cannot alter a protection setting.

The first physical firmware read on 2026-07-30 returned:

| Register | Verified value |
| :--- | ---: |
| `0x145` discharge-overcurrent warning | 36.0 A |
| `0x146` protection 1 | 45.0 A |
| `0x147` protection 1 delay | 1000 ms |
| `0x148` protection 2 | 60.0 A |
| `0x149` protection 2 delay | 500 ms |

These values explain the ride interruption precisely. The BMS warning begins
below the two-controller 40 A nominal combined VESC input limit, and protection
1 is only 5 A above it. The ride log reached 46.8 A VESC battery current; DALY's
2 Hz sample showed about 44.9 A immediately around the transition before
opening the discharge MOS. A threshold crossing between DALY samples and the
one-second delay are consistent with the observed protection-1 flag. This is
diagnosis, not permission to raise the BMS limits: motor-controller limits,
cell/pack current ratings, BMS rating, wiring, connectors, and desired transient
headroom must be considered together before changing either side.

## 3. Telemetry (Characteristics `0001` + `0004`)
Since fw 0.9.4 telemetry is split across two notify characteristics so neither
payload can approach the BLE notify size limit (a notify silently truncates past
ATT_MTU−3; the old merged payload measured ~503 bytes mid-ride):

- **`0001` core, 5 Hz (every 200 ms):** the fast-changing ride fields — `live`,
  `vesc`, `blive`, `bsrc`, `mph`, `spd`, `bat`, `v`, `w`, `mtr_t`, `esc_t`, `btemp`, `rng`,
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

`live` continues to mean **drivetrain/VESC telemetry**. `blive` independently
means a fresh battery view is available, and `bsrc` is `vesc`, `daly`, `fused`,
`demo`, or `none`. When the VESC is off but the Daly remains reachable,
`live:false` and `blive:true`: `bat`, `v`, `btemp`, `rng`, and `cellv` remain
useful while speed, watts, motor amps, duty, and motor/ESC temperatures stay
masked. Non-BMS builds fall back exactly to the original VESC behavior.

For a fresh Daly link, `v` consistently uses the Daly pack measurement whether
the VESC is on or off. This prevents an ESC power transition from looking like
a real battery-voltage jump; the raw VESC input voltage remains authoritative
for drivetrain safety, loaded-voltage tracking, power, and diagnostics.

The installed Daly coulomb counter was physically verified on the 2026-07-30
joint ride. With a fresh in-range counter, `bat` and remaining range use
`0006.soc`. If it is unavailable, complete Daly cell data falls back to a
weakest-cell voltage estimate; without that, a live VESC uses the existing
voltage model. Those voltage fallbacks are marked provisional in diagnostics.
The JSON shape is unchanged.

**Example Payload:**
```json
{
  "live": true,      // Current live values are valid
  "vesc": true,      // Recent real VESC packet received
  "blive": true,     // Fresh battery data available independently of drive data
  "bsrc": "fused",   // vesc | daly | fused | demo | none
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
| `calWh` | int | ❌ | fw 0.9.5+: qualified range-model pack energy, Wh; `0` until a safe learning window is available; not a battery-health result |

> **Range authority (fw 0.9.5+):** when `calR` is present the board learns its
> own range model on-device (consumption, pack IR, qualified pack energy). The app
> must not auto-push a learned `whmi` over it — explicit user-initiated writes
> remain fine. Storage schema v3 invalidates legacy `calWh` values learned from
> loaded voltage-derived SOC. New pack-energy learning requires a near-full
> baseline, at least a 15% SOC window, and a stationary low-current endpoint held
> for 20 s. BMS builds currently keep pack-energy learning disabled because the
> legacy learner's endpoint SOC is voltage-derived while the rider-facing SOC is
> Daly-derived; consumption (Wh/distance) and pack-resistance learning remain
> active. Do not mix those SOC domains merely because current is now validated.

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
| `"PAGE_SET:<n>"` | Jumps the display to an **absolute** page index (0=HUD,1=DASH,2=POWER,3=TRIP,4=SETTINGS,5=SYSTEM,6=GRAPHS,7=LOGS,8=BMS when built). Use this for page-swipe sync when the app's page set doesn't 1:1 the board's. |
| `"BRIDGE_MODE"` | Requests VESC Tool Bridge Mode. On boards with buttons the display shows **START BRIDGE? L=YES R=NO** — the rider must physically confirm (30 s timeout) because the BLE link is unauthenticated and the bridge exposes motor config. Buttonless builds enter directly. |
| `"WIFI_EXPORT_START"`| Requests the ESP32's WiFi AP + HTTP server (logs + OTA) without entering VESC bridge mode — telemetry/BLE keep streaming. On boards with buttons the display shows **ALLOW WIFI? L=YES R=NO** (30 s timeout); the AP only rises after the rider confirms. |
| `"WIFI_EXPORT_STOP"` | Drops the standalone WiFi AP / HTTP server started by `WIFI_EXPORT_START`. |
| `"CAL_RESET"` | Wipes the board's learned range calibration so a replacement battery can be learned from scratch. |
| `"UNSET:<key>"` | Removes a persisted rider override and immediately falls back to the VESC base (or generic default). Allowed keys: `cells`, `packAh`, `stopCell`, `homeCell`, `whmi`, `wheelmm`. Use this for a companion-app **Use VESC** action; do not write the VESC number back as another override. |
| `"REBOOT"` | Reboots the ESP32 display. |

## 6. Hybrid WiFi Session-Log Transfer (Bulk Data)
The ESP32 writes one board session CSV from dashboard boot until power-off/reboot. It records at 1 Hz while the dashboard is running and is not reset by `TRIP_RESET` or phone trip recording. Because session logs can easily exceed 500 KB, attempting to download files this large over BLE is mathematically slow and prone to timeout errors.

Current `tdisplay_s3_bms` logs identify themselves with the comment
`# esk8os_session_schema=4`. VESC-active, charging/discharging, high-delta, and
alert states log at 1 Hz; a stable parked BMS uses a 10 s heartbeat and a
both-links-down state uses 30 s. Link, alarm, MOS, and large-delta changes write
immediately. This prevents an overnight USB-powered bench session from rotating
out ride evidence while preserving full ride/charge detail. Each row appends:

`bms_link,bms_pack_v,bms_current_a,bms_soc_pct,bms_remaining_ah,`
`bms_delta_mv,bms_min_cell,bms_min_mv,bms_max_cell,bms_max_mv,`
`bms_max_temp_c,bms_charge_mos,bms_discharge_mos,bms_fault,`
`bms_stale_mask,bms_fault_hex,bms_cells_mv,bms_valid_mask,bms_alarm_level,`
`battery_view_v,battery_view_soc,battery_view_source,`
`battery_soc_provisional,battery_range_home_km`

`bms_stale_mask` is hexadecimal with bit 0 = cell 1. `bms_fault_hex` preserves
the seven raw `0x98` fault bytes. `bms_cells_mv` is an index-ordered,
pipe-delimited millivolt list (cell 1 first); it is populated every 10 s and on
BMS link/fault transitions or a delta change of at least 25 mV, and otherwise
left empty. This keeps the active rollup useful without duplicating the full cell
array on every row. `bms_valid_mask` matches characteristic `0006`'s `fv`;
`bms_alarm_level` matches `al`. The `battery_view_*` columns record exactly what
the UI/`0001` battery view selected, alongside the raw VESC and Daly fields, so
an on/off source transition and range estimate can be audited after a ride.
Event values identify verified protection transitions as
`bms_discharge_oc_warn` and `bms_discharge_oc_protect`; a later clear remains
`bms_alarm_clear`. Trip resets are also attributable:
`trip_reset_button`, `trip_reset_ble`, `trip_reset_console`,
`trip_reset_remote`, or `trip_reset_auto_parked`. These explicit markers make
an unexpected reset distinguishable from a power loss or BMS cutoff.
When the read-only `0x145..0x149` query first succeeds, the log also writes one
parseable comment:
`# bms_discharge_oc_settings=warning_a:...,protection1_a:...,`
`protection1_delay_ms:...,protection2_a:...,protection2_delay_ms:...,`
`source:read_only_modbus_0x145_0x149`.
It is not repeated in every row, so the CSV column schema remains version 4.
Schema 3 is the same BMS evidence without these five final battery-view columns;
older BMS logs are schema 2; non-BMS builds identify the original column set as
schema 1.

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
