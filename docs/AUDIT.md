# ESK8OS / EVEE firmware audit

A full read-through of the firmware, started 2026-07-15 ahead of the Daly BMS
integration — the BMS work will touch the telemetry model, BLE, and display, so
the ground under it should be verified first.

This is a **living document**: it's built up module by module. Coverage is
tracked below; findings are numbered, ranked by severity, and each names a file,
the failure it enables, and a fix. Check items off as they're addressed.

## Methodology

Audited by risk, not alphabetically. This firmware controls a vehicle a person
rides, so the order is:

1. **Safety-critical** — throttle, failsafes, VESC comms, the math that tells a
   rider it's safe to keep going. A bug here puts someone on the ground.
2. **Concurrency & robustness** — FreeRTOS tasks, shared globals, radio/UART
   contention, boot and link-loss handling.
3. **Correctness & quality (non-safety)** — rendering, console, settings, logs.

Severity: **S1** rider-safety / data-loss · **S2** functional bug · **S3**
robustness/edge case · **S4** quality/maintainability.

## Coverage

| Module | LOC | Tier | Status | Verdict so far |
|---|---:|:---:|:---:|---|
| `transports/VescProtocol.cpp` | 388 | 1 | ✅ reviewed | Solid — bounds-checked, CRC-validated |
| `services/remote_link.cpp` | 424 | 1 | ✅ reviewed | Solid — one minor torn-read note (F-1) |
| `transports/VescUartTransport.cpp` | 605 | 1 | 🔶 partial | Poll/throttle path reviewed; rest pending |
| `telemetry/telemetry.cpp` | 716 | 1 | ⬜ pending | Range/sag/safety math — not yet read |
| `transports/DalyBms.cpp` | 245 | 2 | ✅ reviewed | Decode correct; F-2/F-3 + hardware-verify list |
| `services/companion_ble.cpp` | 708 | 2 | ⬜ pending | BLE, shared state, radio coexistence |
| `services/bridge.cpp` | 244 | 2 | ⬜ pending | VESC-Tool passthrough |
| `config/Settings.cpp` | 270 | 2 | ⬜ pending | NVS, config tiers |
| `ui/ui.cpp` | 1610 | 3 | ⬜ pending | Rendering |
| `ui/UiRenderer.cpp` | 797 | 3 | ⬜ pending | Rendering |
| `util/console.cpp` | 929 | 3 | ⬜ pending | Console commands |
| `logging/sessionlog.cpp` | 249 | 3 | ⬜ pending | Ride logs |
| `services/webexport.cpp` | 212 | 3 | ⬜ pending | WiFi export UI |
| everything else | — | — | ⬜ pending | |

(`ui/BebasNeue*.h` ≈ 6.4K LOC are generated font tables — excluded.)

## Findings

### Reviewed clean

- **`VescProtocol.cpp` — framing & parsing.** `transact()` bounds every write
  (`plen > maxReply` guard before filling `reply[]`, `len > 250` guard on TX),
  validates the CRC and stop byte, and resyncs past line noise instead of
  trusting the first start byte. `parseValues()` guards every field decode with
  a `has(bytes)` remaining-length check, so a short reply (older firmware, fewer
  trailing fields) stops cleanly instead of over-reading. No buffer issues found.
- **`remote_link.cpp` — arming & failsafe.** Two independent failsafes (150 ms
  link timeout → coast, plus the VESC's own UART timeout). Coast means zero
  current, never brake. Arming needs a continuous 500 ms neutral run on an
  *encrypted* link — a broadcast/unpaired link can't arm. The boot-id in the
  header resyncs the seq counter on a remote reboot *and* forces disarm.
  Strictly-increasing seq rejects stale/replayed throttle. This is careful work.
- **`DalyBms.cpp` — protocol decode.** Every command's byte offsets (0x90–0x98)
  re-verified against the Daly UART spec: the 30000 current offset, the 40 temp
  offset, the 0x95 three-cells-per-frame layout with the 1-based frame number in
  the data byte. Frame read is bounds-safe (`out[13]`, fills `out[1..12]`; decoders
  read within the 8 data bytes). Checksum is the sum of the first 12 bytes. All
  correct. Two robustness issues (F-2, F-3) and a hardware-verify list below.

### Open findings

#### F-2 · S3 · `DalyBms.cpp:223` — BMS snapshot published as a non-atomic copy

`bmsPollTask` builds a local `BmsData b`, then `gBms = b`. The in-code comment
says this avoids publishing a half-finished poll — true — but the struct
assignment *itself* isn't atomic. A UI/BLE reader on another task can land
mid-copy and read a `gBms` that mixes old and new fields (e.g. a fresh pack
voltage with stale cell array).

- **Impact:** cosmetic today — BMS data is monitoring, so a torn frame is one
  bad display update. **But** if `hasFault` / `dischargeMos` are ever read to
  drive an action (cut throttle on a BMS fault, warn-and-latch), a torn read
  becomes a correctness bug on a safety path.
- **Fix:** double-buffer two `BmsData` and publish by swapping a `volatile`
  index the reader loads once; or a seqlock; or a short mutex around publish+read.

#### F-3 · S4 · `DalyBms.cpp:144,164` — stale per-cell data isn't flagged

`readCellVoltages` / `readCellTemps` write only the cells whose frames arrive. A
dropped 0x95/0x96 frame leaves that cell's previous mV/temp in place with no
indication it's stale. The whole point of per-cell readout is catching one
lagging cell — stale data could mask exactly that.

- **Fix:** stamp a per-cell `lastSeenMs` (or clear to 0 at cycle start and treat
  0 as "no reading"), and let the display/app show unknown rather than old.

#### F-1 · S3 · `remote_link.cpp` — torn read of throttle/flags across tasks

`onRecv()` (WiFi task) writes `rxThrottle` / `rxFlags` / `rxButtons` / `rxLastMs`
together; `tick()` (poll task) reads them as separate volatile scalars. Each read
is atomic on its own, but a `tick()` that lands mid-update can pair a *new*
throttle with *old* flags for one tick — so a freshly-set `KILL`/`FAULT` could be
acted on one control tick (~10 ms) late.

- **Impact:** bounded — at most one ~10 ms tick, and the value-side always sends
  zero on fault, so a torn read can't invent throttle. Low risk, but on the
  safety path it's worth removing the ambiguity.
- **Fix:** publish the control packet as one unit the reader consumes atomically
  — e.g. a small seqlock (write an even/odd counter around the copy, reader
  retries on mismatch), or double-buffer the `EveeControl` and swap an index.
  Then `tick()` reads a coherent snapshot.

### Verify on the physical pack (before trusting the BMS readout)

The decode is right *per the spec*, but Daly variants differ in firmware. When
the pack arrives, confirm on-device (the `bms` console command dumps everything):

- **0x95 / 0x96 multi-frame:** the code sends one request and reads
  `ceil(cells/3)` frames. Confirm this Daly streams all cell frames after a
  single request rather than one-frame-per-request. If it's the latter,
  `readCellVoltages` only ever fills the first three cells.
- **Cell/temp counts (0x94):** confirm `cellCount` reads 10 for the 10S pack (the
  code falls back to `BMS_CELL_COUNT_DEFAULT` = 10 until 0x94 answers).
- **Current sign:** confirm `+` = charging, `−` = discharging on this unit.
- **Response address byte:** `readFrame` requires `out[1] == 0x01`; confirm the
  pack answers with 0x01 and not another address.

#### F-1 · S3 · `remote_link.cpp` — torn read of throttle/flags across tasks

`onRecv()` (WiFi task) writes `rxThrottle` / `rxFlags` / `rxButtons` / `rxLastMs`
together; `tick()` (poll task) reads them as separate volatile scalars. Each read
is atomic on its own, but a `tick()` that lands mid-update can pair a *new*
throttle with *old* flags for one tick — so a freshly-set `KILL`/`FAULT` could be
acted on one control tick (~10 ms) late.

- **Impact:** bounded — at most one ~10 ms tick, and the value-side always sends
  zero on fault, so a torn read can't invent throttle. Low risk, but on the
  safety path it's worth removing the ambiguity.
- **Fix:** publish the control packet as one unit the reader consumes atomically
  — e.g. a small seqlock (write an even/odd counter around the copy, reader
  retries on mismatch), or double-buffer the `EveeControl` and swap an index.
  Then `tick()` reads a coherent snapshot.

## Next passes

- Finish Tier 1: `telemetry.cpp` (range/sag/cell-voltage math — these drive the
  "keep going?" decision) and the rest of `VescUartTransport.cpp`.
- Tier 2: **`DalyBms.cpp` before the pack arrives** (frame parse, the 0x95
  multi-frame cell read, the poll task's interaction with the throttle task),
  then `companion_ble.cpp` and `bridge.cpp`.
- Tier 3: rendering, console, settings, logging.
