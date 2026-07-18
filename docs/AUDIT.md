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
| `transports/VescUartTransport.cpp` | 605 | 1 | ✅ reviewed | Solid; mcconf parse clean, publish() mutex correct. F-4 on the two un-mutexed struct handoffs |
| `telemetry/telemetry.cpp` | 716 | 1 | ✅ reviewed | Math solid — div-guards, sag signs, OCV LUT, alert ladder all correct. Single-task, so no torn reads |
| `transports/DalyBms.cpp` | 245 | 2 | ✅ reviewed | Decode correct; F-2/F-3 + hardware-verify list |
| `services/companion_ble.cpp` | 708 | 2 | ✅ reviewed | Deferred-mutation architecture is the reference pattern. F-5 (flash I/O in a BLE callback), F-6 |
| `services/bridge.cpp` | 244 | 2 | ✅ reviewed | Good safety gate + pause/flush. F-7: armed-remote UART contention on EVEE_LINK builds |
| `config/Settings.cpp` | 270 | 2 | ✅ reviewed | Corrupt-guards + tier ordering solid. F-8: cell-count range mismatch bites >14S packs |
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
- **`telemetry.cpp` — the "keep going?" safety math.** The high-value code. Every
  division is guarded before it runs (`whPerKm` floored at `defaultWhPerKm()*0.6`,
  `sessionDistanceKm >= learnDist`, `dKm >= 0.5`, `socDrop >= 10`,
  `sessionMovingSec > 0`), so no path produces NaN/Inf into the range figures.
  Sag compensation signs are correct throughout: `vOpen = V + I*R` adds sag back on
  discharge and removes it on regen; the pack-IR estimate `-dV/dI` yields a positive
  R on a current step-up; `effectiveFloorCellV` lifts the resting floor by per-cell
  sag so "range to limp" means range until it actually limps. The OCV→SoC LUT is
  monotonic and interpolated (fixing the old linear over-read near empty). The
  `rangeAlertState` ladder is ordered LIMP → SAG → TURN-HOME and correctly gated
  (`!telemetryLive` clears it; the voltage tripwires require `discharging`; the
  range tripwire is an ungated backstop). No bug found in the math.
- **`VescUartTransport.cpp` — mcconf parse & publish path.** `parseMcconfBase`
  bounds every field read behind `len < 456` and refuses any signature but
  `MCCONF_SIG_6_05` (no guessing on unknown layouts); `beF32Auto` matches bldc's
  `buffer_get_float32_auto` exactly; the sanity gate rejects a partially-plausible
  parse wholesale. `publish()`/`getLatestVescData()` correctly serialize the
  `gRawData` handoff through `gDataMutex`. Two sibling structs miss that mutex — see
  F-4.
- **`companion_ble.cpp` — task discipline.** This is the *reference* pattern the
  other transports should copy. The NimBLE host task never mutates app state
  directly: `onWrite` (settings/command) copies the payload into `g_setBuf` /
  `g_cmdBuf` under a `portMUX` critical section and sets a pending flag; the UI
  task drains it in `companionBleTick` under the same lock and does all the heavy
  work (GFX, flash, `updateRangeEstimate`, `ESP.restart`) on-thread. The
  telemetry NOTIFY builders run on the UI task too, alongside `pollVescData`, so
  every value in a frame is from one coherent poll. MTU/notify-size limits are
  actively guarded (split 5 Hz core / 1 Hz session chars, serial warnings before
  the 509-byte notify cliff). Two issues on the `onRead` side — F-5, F-6.
- **`bridge.cpp` — VESC-Tool passthrough.** `enterBridgeMode` gates on a stopped
  board (`currentSpeedKmh <= 1.0`), tears down a conflicting export AP first,
  checkpoints odo + flushes the session log, drains stale UART bytes, and pauses
  the telemetry poll before taking the wire — careful sequencing. `bridgeLoop`
  reads `Serial1` once and fans out to whichever transports are connected (never
  double-reads), with a 3-min idle timeout that OTA-in-progress correctly
  suppresses. All on the UI task. One gap: it pauses the *poll* but not the
  *throttle* loop — F-7.
- **`Settings.cpp` — NVS config tiers.** The tier order (rider override > VESC
  base > compiled default) is implemented correctly: overrides load first, then
  `applyBaseTier` fills only keys the rider hasn't set (`!prefs.isKey`). Every
  restored value is `constrain`-clamped, and the two learned-energy values have
  explicit corrupt-guards (out-of-band → reset to the "unlearned" sentinel)
  *specifically* so a bad NVS read can't feed a near-zero Wh/km into the range
  math and blow it up to infinity — the same defensive instinct as the
  telemetry div-guards. Odo/trip/moving-time are preserved unconditionally across
  a schema bump (the fix for an old mileage-wipe bug). All NVS writes are on the
  UI/boot task; the only cross-task NVS touch is F-5's `sourceTag`. One
  cross-layer range mismatch — F-8.

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
- **✅ Fixed.** A `portMUX` (`rxMux`) now brackets both sides: `onRecv` publishes
  all four control fields inside a critical section, and `tick()` takes one
  coherent snapshot of them (plus `rxRestarted`) at the top and uses locals
  throughout. Behaviorally identical to before except the torn-read window is
  gone. **Throttle-path change — verify on-device (wheels off the ground, the
  `_evee_link` dry-run env) before any live ride**, per the repo's own protocol.
  Compiles clean under `tdisplay_s3_evee_link`.

#### F-4 · S3 · `VescUartTransport.cpp:196,521` — two struct handoffs skip the mutex the third one uses

The task correctly publishes `gRawData` through `gDataMutex` (`publish()` /
`getLatestVescData()` both take it) — that's the reference pattern. But two other
structs cross the same core 0 → core 1 boundary with a bare assignment and a
field-by-field read on the far side:

- **`gStats`** — `pollStats()` (core 0) does `gStats = s`; `getVescRideStats()`
  (core 1) reads ten fields out of it unlocked. A reader landing mid-copy gets a
  ride-stats card mixing old and new avg/max values. Cosmetic; refreshes ~2 s.
- **`gVescBase`** — `parseMcconfBase()` (core 0) fills it field-by-field then sets
  `.valid` last; `getVescBaseConfig()` (core 1) checks `.valid` then copies. Cross-core
  with no barrier, the reader can observe `.valid == true` before the field stores are
  visible, or copy mid-write. It's written exactly once per boot and read one-shot into
  `applyVescBase()`, so the window is tiny — but the payload seeds motor poles / gear /
  wheel size, i.e. the **speed scale for the whole session**, so a torn read isn't purely
  cosmetic. Low probability, real consequence.

Same root cause as F-1/F-2 (see synthesis below). Diagnostic-only siblings worth a
line: the terminal passthrough (`gTermText`, handed off on a `volatile` `gTermState`
flip) and the mcconf status flags publish across cores without a barrier too — log/USB
paths only, so no rider impact, but they belong in the same one-shot fix.

- **Fix:** route `gStats` and `gVescBase` through the existing `gDataMutex` (or a
  tiny `publishStruct`/`readStruct` helper), exactly as `gRawData` already is.
- **✅ Fixed.** Both now publish and read under `gDataMutex`: `parseMcconfBase`
  fills a local and swaps it in under the lock; `pollStats` publishes
  `gStats`+`gStatsHave` under the lock; `getVescBaseConfig` copies under the lock;
  `getVescRideStats` snapshots under the lock then converts outside it. Diagnostic
  siblings (terminal/mcconf-status flags) left as-is — log/USB only, no rider path.

#### F-5 · S3 · `companion_ble.cpp:230` — a BLE read callback does synchronous flash I/O

`BaseConfCallbacks::onRead` → `buildBaseConfJson` → `Settings::sourceTag` →
`prefs.isKey(...)` performs **NVS (flash) reads on the NimBLE host task**, every
time the app reads the base-config characteristic. Meanwhile the UI task writes
NVS on its own schedule — `saveOdo` every 60 s and on each stop edge,
`applySettings` on any settings write. ESP-IDF's NVS layer is internally locked
per-partition, so this does **not** corrupt anything. But:

- **Impact:** an `onRead` that lands during a UI-task flash commit blocks behind
  the NVS lock — and a page erase is tens of ms — stalling the BLE host task and
  delaying or dropping notifies for that window. It also violates the rule the rest
  of this file follows so carefully (no blocking I/O in a radio callback), so
  it's a latent hazard if the storage layer ever changes. The settings `onRead`
  is clean by comparison — `wifiPassword()` caches in a static buffer, so that
  path is RAM-only; only the baseconf provenance tags reach flash.
- **Fix:** compute the four `sourceTag` provenance letters on the UI thread
  (they only change on a settings write) and cache them, or snapshot the
  `isKey` results into RAM at settings-apply time. Then no `onRead` touches flash.
- **Deferred (not yet fixed).** Bounded in practice — ESP-IDF's NVS keeps an
  in-RAM key index, so `isKey` is a RAM lookup under the partition lock, not a
  raw flash read; the exposure is lock contention behind a UI-task write, not
  corruption. The clean fix (cache the tags on the UI thread) adds API surface,
  so it's grouped with the `companion_ble` `onRead` follow-up rather than rushed
  into this pass.

#### F-6 · S4 · `companion_ble.cpp:230,417` — onRead builders read app globals unsynchronized

Both `onRead` builders run on the BLE host task and read app globals
(`gPackROhm`, `gThemeIdx`, `BATTERY_*`, `gBase`, …) that the UI task writes. A
read that interleaves with a settings-apply gets a JSON snapshot mixing pre- and
post-change fields. Each field is a single aligned word (atomic on the ESP32), so
there's no torn *value* — only a momentarily inconsistent *set*, refreshed on the
next read. Cosmetic, same benign class as the telemetry cross-field reads. Left as
S4 unless a future characteristic value is ever used to drive an action.

#### F-7 · S3 · `bridge.cpp:228` — bridge mode doesn't stop the throttle loop's UART writes

`enterBridgeMode` calls `setVescPollPaused(true)` and hands `Serial1` to VESC Tool
— but on `EVEE_LINK_ENABLED` builds `RemoteLink::tick(gProto)` runs at the *top*
of `vescPollTask`'s loop, **before** the `gPollPaused` gate, on every control
tick. When the ESP-NOW remote is armed, `tick()` writes throttle frames to
`Serial1` — the very wire `bridgeLoop` (UI task) is reading and forwarding VESC
Tool traffic on. Two writers on one UART interleave bytes: exactly the corruption
`VescUartTransport`'s "sole owner of the UART" design exists to prevent.

- **Impact:** conditional — needs an *armed* remote during a bridge session. At a
  bench config that's unusual (the remote is normally off/disarmed), and the
  VESC's own UART timeout coasts on a garbled throttle stream, so it's bounded.
  But nothing *prevents* it, and the failure (corrupt mcconf writes from VESC
  Tool, CRC-retry storms, or a mis-decoded throttle) is nasty to diagnose.
- **Fix:** gate `RemoteLink::tick` on `!gPollPaused` too, or force-disarm the link
  on bridge enter, so bridge mode is genuinely the sole `Serial1` owner. Pairs
  naturally with the F-1 remote-link work.
- **✅ Fixed.** `vescPollTask` now computes `linkArmed = gPollPaused ? false :
  RemoteLink::tick(gProto)`, so the throttle loop makes zero `Serial1` writes
  while bridge mode owns the wire. `gPollPaused` is now `volatile` (cross-task
  read). One residual edge left as-is: if the remote is armed at the instant of
  bridge *exit*, the state machine resumes into `ARMED` — but exit only happens
  on a stopped board at the bench, so it's out of F-7's UART-contention scope.

#### F-8 · S3 · `Settings.cpp:195,199` — cell-count accepted range disagrees across layers

`parseMcconfBase` accepts a VESC `cells` value of **3–24**; `applyBaseTier` only
copies it into `BATTERY_CELLS_COUNT` when it's **6–14**; the BLE settings write
also clamps 6–14. But `applyBaseTier` still derives `stopCell`/`homeCell` from the
*raw* `gBase.cells` regardless. So a >14S (or <6S) pack lands in an inconsistent
state: the per-cell voltage floors are computed from the true cell count, while
`BATTERY_CELLS_COUNT` — the divisor telemetry uses for `loadedCellVoltage` and the
SoC curve — stays pinned at the default 10. Per-cell voltage then reads roughly
`realCells/10 ×` wrong, breaking SoC, sag, and the whole range/alert chain.

- **Impact:** none on the current 10S hardware. But `VT_CAR` / `VT_EMOPED` are in
  the vehicle enum and the platform vision is explicitly multi-vehicle — a 20S
  e-moto is squarely in the roadmap, and it would hit this. Latent, not abstract.
- **Fix:** make the three ranges agree — either widen `applyBaseTier` / the BLE
  clamp to the parser's 3–24, or narrow the parser. Whatever the supported span
  is, one constant should define it, and `BATTERY_CELLS_COUNT` must never diverge
  from the cell count the voltage floors were derived from.
- **✅ Fixed.** `BATTERY_CELLS_MIN`/`MAX` (6/14) now live in `esk8os.h` and are
  the single definition used by the NVS load, the BLE settings clamp, and
  `applyBaseTier`. `applyBaseTier` gates *every* cell-derived value
  (`BATTERY_CELLS_COUNT`, `stopCell`, `homeCell`) on the same `cellsUsable` check,
  so an out-of-range pack falls back to defaults wholesale instead of a mixed
  state. The mcconf parser keeps its wider 3–24 so drivetrain values still apply.

### Concurrency synthesis — how pervasive is the non-atomic publish?

The handoff flagged F-1/F-2 as one bug (a multi-field struct handed between
FreeRTOS tasks without an atomic publish) and guessed "the telemetry globals do
it too." This pass settled the scope, and the news is mostly good:

- **The telemetry globals are single-task — no bug there.** `pollVescData`,
  `recordHistorySample`, the renderer, `companionBleTick`, and every
  `getHistorySample` all run on the core-1 `loop()` (`App.cpp:611` →
  `dashboardLoop`). Only two tasks exist (`vescPollTask`, `bmsPollTask`, both
  core 0), and *neither reads the telemetry globals*. So `currentSpeedKmh`,
  `rangeAlertState`, `vescFault`, `history[]` etc. are never read cross-task —
  the guess that they'd need atomic publish was wrong.
- **The safety outputs would be atomic anyway.** `rangeAlertState` and
  `vescFault` are single `int`s; even if they were shared, a scalar load can't
  tear. The decision surface isn't a struct.
- **So the real scope is four sites, not "pervasive":** F-1 (`remote_link`
  control packet, 100 Hz, safety path), F-2 (`DalyBms` `gBms`), and F-4
  (`gStats` + `gVescBase`). All four are the *same* cross-core struct handoff.
- **The fix pattern already exists in-tree.** `VescUartTransport::publish()` +
  `gDataMutex` is the correct reference. Reuse it for F-2 and F-4 (cold/warm
  paths where a mutex is free). For F-1 alone — a 100 Hz control loop where a
  mutex on the hot path is undesirable — use the seqlock/double-buffer the F-1
  entry describes. One shared helper, four call sites.

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

## Next passes

- **Tier 1 is done.** `VescProtocol`, `remote_link`, `VescUartTransport`, and
  `telemetry` all reviewed. Safety math verdict: solid — every division is
  guarded, sag compensation signs are right, the OCV→SoC LUT interpolates
  correctly, and the range-alert ladder is ordered most-severe-first with the
  right `telemetryLive`/`discharging` gates.
- **Concurrency fixes — mostly done.** ✅ F-1 (`rxMux` snapshot), ✅ F-4
  (`gStats`/`gVescBase` under `gDataMutex`), ✅ F-7 (throttle tick gated on
  `!gPollPaused`) are fixed and build clean. **F-1 still needs the on-device
  wheels-off-ground check** before a live ride. **F-2** (Daly `gBms`) is
  deliberately deferred to the BMS integration — the pack isn't physically
  connected, so the fix can't be exercised, and it's better done while the read
  path is being built for real BMS data. ✅ F-8 (cell-count range) also fixed.
- **Tier 2 is done.** `companion_ble.cpp`, `bridge.cpp`, `Settings.cpp` all
  reviewed (`DalyBms.cpp` was already done). The concurrency picture is now fully
  mapped — see the synthesis; F-1/F-2/F-4/F-5 are the remaining task-safety items.
- Tier 3 (quality, non-safety): `ui.cpp` (1610), `UiRenderer.cpp` (797),
  `console.cpp` (929), `sessionlog.cpp`, `webexport.cpp`. Lower risk — rendering
  and console — so this is polish-hunting, not safety.

### On-device verify (non-BMS)

- **WiFi export AP + BLE notify coexistence.** `WIFI_EXPORT_START` brings up a
  SoftAP while BLE stays connected; the ESP32-S3 shares one radio between them.
  The code intends telemetry to "stay live" during export — confirm on-device
  that 5 Hz notifies actually survive with the AP up (coexistence can starve BLE
  airtime). If they hitch, the fix is a lower notify rate while exporting, not a
  code correctness change.
