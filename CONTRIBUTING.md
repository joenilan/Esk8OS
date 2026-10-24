# Contributing to EVEE/ESK8OS

BMS proposals, testing evidence, and technical discussion are welcome. Project
licensing and contribution terms are currently undecided; see
[LICENSING.md](LICENSING.md). Earlier options are archived drafts, not adopted
terms. Contribution permissions should be settled with the maintainer before
outside code is merged; there is no blanket commercial-use grant in this guide.

## Adding a BMS driver

You do not need the maintainer to own your hardware. Bring the exact device,
protocol evidence, fixtures, and reproducible validation for the model you own.
Start with a BMS support issue before a large implementation.

Current verified implementation: **Daly Smart BMS, UART K BLE module**. Other
Daly protocol families and other brands are not implied to work. Three hardware
profiles (TFT, OLED, headless) are independent of the BMS driver. Today the
release matrix is each profile with no BMS or with Daly; a verified new driver
adds another supported driver option, not an assumed universal BMS binary.

1. Put protocol/transport code in its own adapter under `src/transports/`.
   The lifecycle/setup interface and registration live in
   `BmsDriver.h` / `BmsDriver.cpp`. Application boot, persisted target setup,
   snapshot reads, and the BLE driver identity use that interface. Register
   only implemented drivers and make unsupported combinations fail clearly.
2. Preserve controller ownership: one initialized NimBLE stack, BMS transport
   work on its own poll task, bounded callback queues, atomic snapshots, and
   no UART sharing with a bridge or control task. A new driver must not write
   motor or battery protection settings as part of discovery or monitoring.
3. The current `BmsData` schema and legacy raw alarm helpers still live in
   `DalyBms.h` and include Daly-specific evidence. They are **not** a universal
   fault format. The first non-Daly PR must explicitly normalize current sign,
   units, validity, freshness, and alarm severity into shared presentation data,
   preserving provider-specific raw diagnostics separately. Never reinterpret
   another brand's alarm bits with the Daly helper or invent missing values.
   Shared-model extraction should ship with the real driver and fixtures.
4. Persist the rider's explicit target on the ESP32. No personal address/name,
   auto-selection of nearby packs, or hard-coded pack calibration. Keep target
   mutation stopped/dashboard-only, invalidate old pack snapshots on changes,
   preserve existing NVS/ride data, and handle absent/offline hardware.
5. Expose a stable driver ID (currently `daly`) through optional characteristic
   `0007`. Update `docs/companion_api_spec.md` and the separate mobile repo
   deliberately. The app must show the supported type and refuse unknown
   drivers instead of silently using the Daly setup path. BLE vs wired setup
   requirements must be explicit; do not assume every BMS uses BLE addresses.
6. Add the matching PlatformIO environments for TFT, OLED, and headless, then
   register their explicit driver ID in `scripts/publish_release.py`.
   Artifact names must be distinct; filenames do not determine protocol type.
   Do not add placeholder downloads for unimplemented models.

## Evidence and tests for a BMS PR

- Exact vendor/model, BMS firmware version, module/transport, tested cell count,
  and hardware wiring/pins. State limitations and models not verified.
- Protocol documentation or attributable traces you may legally share. Remove
  personal addresses, pairing secrets, GPS routes, and private ride logs.
- Parser fixtures covering valid frames, invalid length/checksum, split and
  coalesced data, invalid cell counts, unknown alarms, stale/incomplete fields,
  reconnect, target change, and missing hardware.
- Expected units and current direction, plus comparisons against the vendor's
  own readouts. Unknown safety evidence must stay unknown or alerting.
- Build results for the hardware combinations actually supported. Separate
  compile-only verification from physical tests of reconnect, phone coexistence,
  forget/reselect, and persistence over a power cycle.

Run the host checks and relevant builds from this checkout:

```bash
python3 -m unittest discover -s tests -v
./scripts/piow run -e tdisplay_s3_bms -e esp32s3_oled_i2c_bms_usb -e esp32s3_headless_bms_usb
~/.local/share/esk8os-platformio/bin/python scripts/publish_release.py --dry-run
```

Native target tests require a C++11 compiler. Optional sanitizer pass:
`CXX=clang++ EVEE_SANITIZERS=1 python3 -m unittest discover -s tests -p test_daly_target.py`.
Use isolated test data; never erase NVS or uninstall a rider's app to test a PR.

## Review and releases

Explain the supported device and behavior, attach evidence, and complete the
PR checklist. Build success is not hardware or security certification. Keep
unrelated changes out of the PR and preserve prior license/attribution notices.
Maintainer review determines when a contributed driver becomes a supported
release download. Contributors do not need deployment credentials or access to
the maintainer's production vehicle, phone, or battery.
