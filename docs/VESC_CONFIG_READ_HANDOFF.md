# Handoff — Read real base config from the VESC (stop guessing)

**Purpose:** Read the VESC's *actual* configuration (battery cutoff start/end, motor
poles, battery/motor current limits, cell count, wheel diameter, gear ratio)
directly from the ESC, instead of the rider hand-entering them in the app where
they get decoupled from the VESC and produce wrong stats. Written for a fresh
agent/session. Everything below is marked **VERIFIED** (with method) or
**UNVERIFIED / rider-reported** — do not blur the two.

---

## The core problem (VERIFIED)
The app's battery/range settings (home floor, limp floor, poles, wheel, cells,
pack Ah) are **entered by the rider and stored in ESP32 NVS**. They are **NOT read
from the VESC.** So the rider maintains the same numbers in two places (VESC Tool
*and* the app) and nothing reconciles them. Any "cutoff voltage" the app shows is
either the rider's app-entered number or an ESP32-*computed estimate* — never the
VESC's real setting.

- Verified by: `src/config/Settings.cpp` (NVS keys `homeCell`/`stopCell`/`wheelprof`/
  `cells`/`packAh` all rider-set), and grep of `src/transports/` showing no config read.

## What the firmware can and cannot do today (VERIFIED)
- Protocol client: `src/transports/VescProtocol.{h,cpp}` + `VescUartTransport.{h,cpp}`.
- Implements ONLY telemetry: `COMM_GET_VALUES` (4), `GET_VALUES_SETUP` (47),
  `GET_VALUES_SELECTIVE` (50), `GET_VALUES_SETUP_SELECTIVE` (51).
- Does **NOT** implement `COMM_GET_MCCONF` (14) or `COMM_GET_APPCONF` (16).
  → The app **cannot read VESC config at all** right now. (grep-verified.)
- The only write command sent to the VESC is `COMM_SET_ODOMETER` (110). The firmware
  has NO setCurrent/setDuty/cutoff commands — it cannot limit or cut the motors.

## The task
Add `COMM_GET_MCCONF` (command 14) to the protocol client and extract the
high-value fields. **Do it evidence-first, in this order — do not skip step 1:**

1. **Dump raw bytes only.** Send payload `{14}` framed by the existing
   `transact()`/CRC path. Print the reply length + full hex to serial. Confirm a
   valid reply (VESC echoes command 14, plausible length ~400–500 B). NO parsing yet.
2. **Read the signature.** The mc_configuration is serialized per
   `confgenerator.c` in VESC firmware (github.com/vedderb/bldc), keyed by a 32-bit
   signature near the start. Match it to the layout for **this board's VESC FW**
   (reported as 6.5 — but re-read it live, don't trust this doc).
3. **Extract only after signature match:** `l_battery_cut_start`, `l_battery_cut_end`
   (float32), `si_motor_poles`, `l_in_current_max`/`l_in_current_min`,
   `l_current_max`/`l_current_min`, `si_battery_cells`, `si_battery_ah`,
   `si_wheel_diameter`, `si_gearing_ratio`. Offsets are VERSION-SPECIFIC — never
   hardcode blind.
4. **Validate every field** against sane ranges AND cross-check with trusted
   telemetry (e.g. `si_battery_cells` ≈ measured pack V ÷ ~4.0). If the signature is
   unknown or a value is implausible, surface "unknown VESC config version" — do
   **not** ship a guessed value.

**Fragility warning:** the reason this wasn't done originally is the mcconf layout
shifts between VESC firmware versions. Signature-matching is mandatory. Reference:
`datatypes.h` (`mc_configuration` struct) + `confgenerator.c` in the bldc repo.

## Test setup (VERIFIED this session)
- Board: LilyGO T-Display-S3, native USB CDC. Enumerates as **COM5** when powered
  (rider powers the ESP32 over USB independently of the pack).
- Serial probe: `python scripts/serial_query.py <cmd ...>` @ 115200. Useful cmds:
  `sys cal trip odo cfg stat diag wheel`.
- **To read mcconf the PACK MUST BE ON** — the VESC must be awake to answer. With the
  pack off, `stat` shows "VESC link DOWN" and there is no config reply.
- **CONSTRAINT (rider-stated 2026-07-03): probing over USB while the pack is on is
  NOT available on this setup.** Do not build workflows that need PC + pack
  simultaneously. Reason not stated — do not speculate. See the status addendum below.
- Console gotcha: `consoleOut()` writes to BOTH `Serial` and `Serial0`; **COM5
  receives `Serial0`**. A plain `Serial.printf` does NOT appear on COM5 — use
  `Serial0.printf` or `consoleOut()`.
- Build/flash: `pio run -e tdisplay_s3_debug_usb -t upload --upload-port COM5`.
  A flash succeeding but the board "not coming back" was, once, just an unplugged
  USB cable — verify COM5 presence before assuming a bad flash.

## Verified board state (methods noted)
- VESC FW **6.5**, dual VESC (master + slave over CAN) — seen on app DIAG page, pack on.
- Trip/odo correct: **trip 14.65 mi, odo 39.49 mi** — verified via `trip`/`odo` serial
  AND an instrumented footer probe reading `totalKm=63.5489`.
- ESP32-LEARNED calibration (these are estimates, NOT VESC config): pack R 91 mΩ,
  typical draw 18.9 A, learned pack energy 455 Wh (label 610 → "75% healthy"),
  consumption 24.6 Wh/mi. Source: `cal` command (`telemetry.cpp`).

## UNVERIFIED / rider-reported — DO NOT treat as fact
- Rider recalls VESC Tool set to **3.4 / 3.1 V/cell** cutoff, cuts at ~31 V, "slows at
  ~85%." This is memory, not read from the VESC. **The whole task is to confirm it.**
- Rider earlier said it "died at 33 V full throttle." The 33 vs 31 discrepancy must be
  resolved by READING the VESC + its cutoff-start-vs-end behavior, not by theorizing.
- Rider reports same distance at 40 A/side vs 15 A/side. Not analyzed against per-ride
  data yet (no controlled two-ride dataset pulled).
- A transient odometer glitch showed **0.39 mi (exactly 100× too small)** on the board
  footer, then self-corrected. Root cause NOT found. Data is safe. A tripwire is armed:
  the footer logs `[AUDIT ODO-CORRUPT]` if lifetime odo ever reads below trip. There is
  also a leftover debug probe in `src/ui/ui.cpp` `updateBottomBar()` — review/remove.

## Repo rules (hard constraints)
- **ALWAYS run `python scripts/archive_sessions.py` BEFORE every flash.** Every
  boot creates a session file and rotation evicts old ones; bench boots destroyed
  the only board-side log of the Jul-3 ride while it was needed as evidence.
  Eviction is stub-first as of `d0a9c3e` (rides survive bench boots), but archive
  anyway — flash storage is not an archive.
- Firmware repo `E:\AI\Longboard-Display` — branch **next-dev**. App repo
  `E:\AI\esk8os_mobile` — branch **main**. Latest audit commits: firmware `76432be`,
  app `a2cecc2` (`git log` for full context).
- **Never** add a remote to / push the parent `E:\AI` repo.
- **Never** `git add -A` in the app repo (a GPS database leaked once). Add named files.
- **Never** put real GPS coordinates on the public site.
- Do not publish 0.9.5 firmware without rider OK (range math is ride-unvalidated).
- BLE settings contract: `docs/companion_api_spec.md`.

## Mistakes made this session (so you don't repeat them)
1. Fabricated a "phone recorded 0.38 mi over GPS" claim the rider never made. The
   0.38/0.39 was the hardware footer.
2. Implied the range floors came from the VESC. They are app-entered + ESP32-computed
   estimates. This is the exact confusion this task removes.
3. Led with "the VESC does the cutoff, not my firmware" in a way that read as deflection.
4. Called features "verified" after checking only the happy path.
**Rule for next agent:** state method with every fact. If it wasn't read from the
device this session, it's a hypothesis — label it one.

---

## Status addendum — 2026-07-03 (step 1 implemented, awaiting a capture)

Because live USB probing with the pack on is unavailable (see constraint above),
step 1 was redesigned as an **offline capture** — code is in the working tree on
`next-dev`, builds on `tdisplay_s3_debug_usb`, `tdisplay_s3_ride_release`, and
`wokwi-simulator`, NOT yet committed, NOT yet flashed (no COM port present when
built; hardware state after the rider's electrical incident unknown):

- `VescProtocol::rawCommand()` — sends a bare one-byte command, returns the raw
  reply payload. No parsing, per step 1.
- `VescUartTransport`: once per boot, after the first successful telemetry cycle,
  the poll task sends `COMM_GET_MCCONF` (14) and writes the reply as a hex dump to
  **`/mcconf.hex`** on LittleFS (up to 10 attempts; a failure is also recorded in
  the file). Power-cycling the board captures again.
- Console command **`mcconf`** — prints capture status for this boot, then the
  saved `/mcconf.hex` (also readable via `cat /mcconf.hex`).

**Retrieval workflow (never combines PC + pack):**
1. Flash `tdisplay_s3_debug_usb` over USB, pack off (rider decides when).
2. Disconnect USB entirely. Power the board from the pack; once the VESC link is
   up the capture lands in `/mcconf.hex` within ~2 s.
3. Pack off. Later, on USB: `python scripts/serial_query.py mcconf`.

Steps 2–4 of the task (signature match against confgenerator.c, field extraction,
validation) are unchanged and operate on the captured hex — do NOT parse until a
real capture is in hand.

### CAPTURE SUCCEEDED + PARSED (same day, method noted per rule)

Board flashed `5159409` (v0.9.6) over COM5 with the pack OFF; the VESC's logic
side was backfed over the COMM harness (documented 3.6 V standby) and answered
`COMM_GET_MCCONF` from standby on attempt 1. Capture: **478 B, cmd echo 0x0E,
signature `0x3F829CF7` = 1065524471 = `MCCONF_SIGNATURE` of bldc `release_6_05`
(fetched from the repo, exact match).** Raw bytes archived at
`docs/captures/mcconf_fw6.5_20260703.hex`.

Parsed OFFLINE with field order auto-extracted from `release_6_05`
`confgenerator.c` (no hand-typed offsets; 477/477 payload bytes consumed
exactly, 197 fields — parser in the session scratchpad, trivially re-creatable).
**READ FROM THE DEVICE (master VESC):**

| field | value |
|---|---|
| `l_battery_cut_start` | **34.0 V** (3.40 V/cell) |
| `l_battery_cut_end` | **31.0 V** (3.10 V/cell) |
| `l_current_max` / `min` | 56.98 / −56.98 A (motor) |
| `l_in_current_max` / `min` | **15.0 / −5.0 A** (battery, per ESC → 30 A total) |
| `si_motor_poles` | 14 |
| `si_gear_ratio` | 4.5 (= 72/16) |
| `si_wheel_diameter` | 0.203 m (203 mm nominal — NOT the measured ~185 mm loaded rolling) |
| `si_battery_cells` | 10 |
| `si_battery_ah` | 16.5 |

Step-4 cross-checks: cells 10 ≈ 42.0 V full ÷ 4.2 ✓; Ah matches configured pack ✓;
poles 14 = firmware's 7 pole-pairs ✓; 15 A/side matches the rider's stated cap ✓.
Rider's recalled 3.4/3.1 cutoffs are **CONFIRMED**. The 33 V full-throttle death is
consistent with cut behavior: the VESC starts ramping current down at 34.0 V
*loaded* and fully cuts at 31.0 V, so a saggy pack under full throttle drops
through cut-start early — that's the "slows down" then "dies" pattern.

Caveats: read from the MASTER only (slave over CAN not captured — would need
COMM_FORWARD_CAN, and whether CAN works in standby is untested); values read in
standby, but mcconf is ESC-side stored config, and all cross-checks pass. Note
the VESC's own speed/odometer use the 203 mm nominal wheel — same ~10% over-read
the display had before wheel calibration.

**Remaining work (next session):** decide how the display/app consume these —
e.g. firmware-side parse of the captured fields (signature-gated, per step 4) to
reconcile/warn when app-entered floors drift from the VESC's real cutoffs.
Currently they happen to agree (app home 3.40 / limp 3.10 = VESC 3.40/3.10).

### "Died at 33 V" RESOLVED from recorded ride data (same day)

Method: pulled the app's trip-backup JSON off the phone (Trip History → backup →
X-plore Copy to → Downloads → adb; file also kept in the rider's phone
Downloads). Jul 3 12:42 ride, 13.52 mi GPS / 14.90 board-mi, 65 min, 3937
telemetry rows at ~1 Hz, zero BLE gaps. **The VESC did not kill the board — the
pack DISCONNECTED at 33.0 V loaded:**

- t+48→64 min: repeated dips into 33–34.4 V under ~700–1100 W. Power near the
  end capped ~700 W vs ~1100 W earlier — matches the VESC ramp math
  ((33−31)/(34−31) = 0.67 of full current). That's the "slows down" feeling.
- t+64.80: voltage collapses **33.0 → 15.3 → 11.5 V in two samples** (not sag —
  a disconnect), rebounds to 36.3 V open-circuit, re-engages ~5 s at 33.x V /
  ~500–650 W, then collapses again 33.1 → 11 → 9 → **6.0 V and stays there**
  (ESC standby backfeed level). Cut → recover → cut = classic BMS
  undervoltage-protection cycling.
- The VESC's 31.0 V hard cut was NEVER reached; a VESC soft-cut also cannot
  make bus voltage collapse — only an opened pack path can.

Interpretation (labeled): most likely the Daly BMS cut discharge because the
WEAKEST parallel group hit its cell-UV threshold — 33.0 V average = 3.30 V/cell,
but this pack is aged/high-IR (learned 91 mΩ) so the weak group sags far below
average under ~20 A. A failing anti-spark could mimic this, but the repeatable
33.0 V trigger point + recover/re-cut pattern fits BMS UV. Practical floor on
THIS pack is therefore ~3.3 V/cell LOADED, above the app's 3.10 limp floor —
range math should respect the BMS, not just the VESC. (Rider's planned binned
10s6p 58E rebuild addresses the root cause.)

Also fixed while here: the `[AUDIT ODO-CORRUPT]` tripwire used `Serial0`
unconditionally, which failed to compile on any env without
`ARDUINO_USB_CDC_ON_BOOT=1` (wokwi verified broken; now guarded in `ui.cpp`).
