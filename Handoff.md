# Evee / Esk8OS Handoff

Date: 2026-06-30 (supersedes the 2026-06-29 version below the line)

> **User preference (important): do not guess.** Inspect code / hardware / docs
> first; verify by building, flashing, or screenshotting before claiming success.
> The user pushes back hard on assumptions — earn conclusions with evidence.

## LATEST — 2026-06-30 (cont.): pairing code + edit indicator + teleport (BUILT, NOT FLASHED)

Same-day continuation. **Round-1 deploy succeeded earlier** (board on COM5 + phone
`RFGL42MHF7Z` both got the prior uncommitted work; serial-verified `fw v0.9.3
ba3bd98-dirty`, name/vtype present, fault 0). Then the user went **remote — board +
phone disconnected, emulator down** — so this round's changes are CODE-COMPLETE and
BUILD-CLEAN but **NOT yet flashed/installed/verified**. Nothing committed.

New this round:
- **Teleport fix** (app `views/trip_view.dart`): marker tween duration is now the
  MEASURED gap between GPS fixes (clamped 250–1500 ms) instead of a fixed 900 ms, so
  the marker stops lagging on fast fixes and snapping forward on pauses. Real-ride
  verification pending (option A, the user's pick).
- **Settings edit-mode indicator** (firmware): root cause was a repaint bug — the
  hold-to-edit toggle (`App.cpp` checkButtons, both buttons) set `gRedrawAll` but
  never called `drawStaticFrame()`, so the existing green/footer indicator never
  repainted. Fixed both; plus a bold inverse-green highlight bar behind the active
  row (`ui.cpp drawSettingLabel`).
- **Pairing-confidence code** (firmware + app): board derives a pair code = BLE MAC
  tail hex (e.g. "EEFF") at BLE init (`companion_ble.cpp computePairCode`, global
  `gPairCode`), advertises `[vtype, macHi, macLo]` in manufacturer data (company
  0xFFFF), shows "PAIR EEFF" on the screensaver + "BLE PAIR #EEFF" on the SYSTEM
  page. App reads the manuf data for the correct vehicle icon + pair code in the
  scan list (`main.dart`), falling back to the MAC tail / skateboard.
- **Scan icon fix** (app): the scan tile hardcoded `Icons.electric_scooter`; now
  `Vehicle.icon(advertised vtype)`. Folded into the pairing advertisement above.

Build artifacts ready: firmware compiles+links clean (`tdisplay_s3_debug_usb`,
flash 19.2%); `app-release.apk` (54.9 MB, release-signed) built; `flutter analyze`
clean.

**WHEN HOME (devices reconnected):**
1. Re-detect the board's COM port (S3 native-USB CDC re-enumerates after reset),
   then `pio run -e tdisplay_s3_debug_usb -t upload --upload-port <COMx>`.
2. `adb -s RFGL42MHF7Z install -r E:\AI\esk8os_mobile\build\app\outputs\flutter-apk\app-release.apk` (preserves trip data).
3. Verify: screensaver shows PAIR code; SETTINGS hold → green edit highlight +
   "TAP -/+ HOLD = done"; SYSTEM page shows BLE PAIR #; app scan list shows the
   skateboard icon + matching PAIR # for the board; real ride for the teleport fix.
4. Then commit (strategy TBD with user).

## Repos & environment

- Firmware: `E:\AI\Longboard-Display` (PlatformIO / Arduino, ESP32-S3)
- Android app: `E:\AI\esk8os_mobile` (Flutter)
- Canonical AI-stack rules: `E:\AI\AGENTS.md` (CLAUDE.md points to it)
- Auto-memory for this project lives at the project memory dir; `MEMORY.md` is
  loaded each session. Key memories: EVEE/ESK8OS branding, vehicle-identity,
  app plan, multi-target hardware, emulator setup, VESC-fault diagnosis.

## Branding direction (decided this session, endorsed by user)

- **EVEE** = umbrella, vehicle-agnostic platform/product name (already the board's
  screensaver logo). **ESK8OS** = the skate-rooted edition / firmware heritage
  underneath. **RideOS rejected** (generic + trademark collision). Rename is
  deferrable — nothing in BLE/UUIDs is tied to "esk8", and the BLE name is now
  user-settable. See memory `evee-esk8os-branding`.

## Hardware state

- LilyGO T-Display-S3 skateboard board = `COM5`.
- User's real Samsung phone = `RFGL42MHF7Z` (**never** target it with adb).
- Android emulator = `emulator-5554` (AVD `esk8_pixel`, S23-spec). **Always pass
  `-s emulator-5554`** to adb — both devices are usually connected at once.
- Generic ESP32-S3 + 0.91" OLED = `COM9` (CH343). OLED is I2C @ `0x3C`
  (SDA=GPIO8, SCL=GPIO9). Do not use VESC UART pins for the display.
- Board runs warm to the touch — that's normal (backlight + BLE radio +
  continuous USB power). NOT caused by the fps work (that *reduced* render load).

---

# THIS SESSION'S WORK (2026-06-29 → 06-30) — ALL UNCOMMITTED

Both repos have extensive uncommitted changes. Nothing was committed. Everything
below is analyzer/​build-clean and verified as noted.

## Firmware (Longboard-Display) — done + verified over serial

- **Smooth screensaver** (`UiRenderer.cpp` `renderTftScreensaver`): float-integrated
  bounce (vx≈48, vy≈33, ~33fps), color flips on wall hits, hides stat corners when
  `!telemetryLive`. Triggers when no live telemetry; wakes on button.
- **Dirty-region blit** (`UiRenderer.cpp`): `pushCanvas()` blits only `markDirty`
  regions vs `pushCanvasFull()`. HUD render dirty-diffed (`ui.cpp` `updateHud`).
  Loop fps 118 → ~500 (delay-bound = mostly idle, not busier).
- **Unpowered-VESC phantom-fault gate** (`telemetry.cpp`): `pollVescData` ignores
  frames with `inpVoltage < VESC_MIN_OPERATIONAL_V (6.0)`; link-lost clears fault.
  (Root cause of mid-ride faults was electrical — 5V rail brownout/back-feed — NOT
  firmware. Recommended a Schottky diode in the harness 5V line.)
- **Session-log hiccup fix** (`sessionlog.cpp`): `sessionLogTick` returns early when
  `!telemetryLive` — no flash writes while idle (they stalled the other core and
  jittered the screensaver).
- **Two-button nav rewrite** (`App.cpp` `checkButtons`): tap (on release) = page
  prev/next; hold (HOLD_MS=400) = contextual action. SETTINGS uses **select-then-edit**
  (hold = toggle `settingsEditing`; tap cycles cursor when navigating, changes value
  when editing). Hold-LEFT on ride pages = toggle units. HUD hold-RIGHT = cycle face.
  TRIP hold-RIGHT = trip-reset-confirm. Both buttons held 2s = bridge mode.
- **Board name + vehicle type** (`esk8os.h`, `Settings.cpp`, `companion_ble.cpp`,
  `console.cpp`): `gDeviceName[20]` (NVS `devname`, advertised BLE name) +
  `gVehicleType` (enum VT_SKATE/EBIKE/ESCOOTER/EMOPED/CAR/OTHER, NVS `vtype`). In the
  BLE settings JSON (`name`, `vtype`) read+write, and console (`name`, `vtype`, shown
  in `cfg`). Name change re-advertises on reboot. **Verified over serial.**

## Android app (esk8os_mobile) — done + verified on emulator

- **Smooth battery gauge** (`esk8_widgets.dart`): `TweenAnimationBuilder` animates the
  fill; the % NUMBER stays the exact telemetry value (smooth but never wrong).
- **Vehicle identity** (`esk8os_ble.dart`, `mock_device.dart`, `esk8_widgets.dart`,
  `settings_page.dart`, `main.dart`): `BoardSettings.deviceName` + `vehicleType`;
  a `Vehicle` helper maps type→`Icons.skateboarding/electric_bike/electric_scooter/
  electric_moped/electric_car`; dashboard top-bar shows the vehicle icon; Settings has
  a **BOARD** section (board-name field + vehicle-type icon picker). Mock supplies them.
  The scan list already shows the advertised name → distinguishes nearby boards.
- **Controls-sheet bug fixes** (`main.dart`): the dashboard quick-controls sheet's
  "BOARD DISPLAY" row no longer overflows (label is `Expanded`+ellipsis); the
  PHONE NAVIGATION buttons use `FittedBox` so "Settings" stops wrapping.
- **Dynamic board-theme parity** (`esk8_theme.dart` + ~10 files) — the headline:
  - `esk8_theme.dart` now holds all **8 board palettes** (RGB lifted verbatim from
    `src/ui/Theme.cpp`), **mutable** live color tokens, `applyTheme(name)`, an
    `isLight` helper, and a `revision` `ValueNotifier`.
  - `Esk8App` wraps `MaterialApp` in a `ValueListenableBuilder(Esk8Theme.revision)`
    with dynamic `ThemeData` (scaffold bg, colorScheme seed, light/dark brightness).
  - `applyTheme(s.theme)` is called wherever settings load (`_fetchSettings` +
    settings page read/write) → instant live re-theme.
  - Fallout fixed: ~32 `const` removals across 9 files (mutable colors can't be in
    `const` widgets); duplicated local `const _accent` unified to a getter
    `Color get _accent => Esk8Theme.accent;` in main/settings/wifi-export.
  - **Verified on emulator:** CYBER mirrors on dashboard+settings (magenta + deep
    purple-black + teal); selecting LIGHT flips the whole app to light mode live.
  - **Intentionally NOT themed:** trip-review pages (`trip_playback_page.dart`,
    `trip_history_page.dart`) keep brand-purple (review context); `trip_overlay.dart`
    CAN'T follow it (separate isolate — no shared Esk8Theme state).

## Open / next work

1. **Recording-map marker teleport** (`trip_view.dart`) — smoothing ALREADY exists
   (`AnimationController _anim` + `_smoothPos` + `_animateMarkerTo`). User reports it
   still teleports during *live recording*. Needs a real moving-GPS trace to diagnose;
   the emulator can mock movement via `adb -s emulator-5554 emu geo fix <lon> <lat>`.
   Don't fix blind.
2. **Phone-native design pass** (not started) — broader UI polish.
3. **(Optional) Unify trip-review accents** to the theme if the user wants them to
   match (currently brand-purple by choice).
4. **On-board verification** of this session's firmware: visually confirm the
   screensaver, two-button nav (select-then-edit settings), and name/vtype on the
   physical T-Display. Serial-verified; on-board UX not yet eyeballed.
5. **Commit** — decide commit/branch strategy with the user; nothing is committed yet.

## Verification workflows

### App (Flutter + emulator)

```powershell
# Build debug APK (Gradle needs the JBR JDK)
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
cd E:\AI\esk8os_mobile; flutter build apk --debug
flutter analyze lib            # keep this clean

$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"; $E='emulator-5554'
& $adb -s $E install -r build\app\outputs\flutter-apk\app-debug.apk   # debug-over-debug OK
& $adb -s $E shell monkey -p com.joenilan.esk8os_mobile -c android.intent.category.LAUNCHER 1
# Enter MOCK mode: tap the bug icon on the scan screen (~895,208 on 1080-wide).
#   May need a couple seconds after launch + a second tap.
# Open Settings: dashboard sliders icon (~985,200) -> controls sheet -> "Settings" pill (~875,1463).
& $adb -s $E shell screencap -p /sdcard/s.png
& $adb -s $E pull /sdcard/s.png <scratchpad>\s.png   # then Read the png
```

- The mock board (`mock_device.dart`) supplies name/vtype/theme and persists to
  shared_prefs — its vtype/theme may be "stale" from earlier runs; that's mock state,
  not a bug. Mock theme is currently `CYBER`.
- Release-signed app vs debug: if `install -r` fails `INSTALL_FAILED_UPDATE_INCOMPATIBLE`,
  `adb -s emulator-5554 uninstall com.joenilan.esk8os_mobile` first. Debug-over-debug is fine.

### Firmware (serial)

```powershell
python scripts\serial_query.py sys cfg stat trip logs   # query T-Display on COM5
python scripts\serial_query.py "vtype 4" "name JOE-DECK" # set + read back
pio run -e tdisplay_s3_debug_usb -t upload --upload-port COM5
```

- Use `tdisplay_s3_debug_usb` for serial (the `_ride_release` env disables USB CDC).
- If COM5 is silent: the 2026-06-29 boot-wait timeout fix means it should reach the
  console even with no VESC; wait ~6s after reset and retry.

## Cautions

- **Only target `-s emulator-5554`** with adb — never the real phone `RFGL42MHF7Z`.
- Do not wipe app data — past trip data matters.
- Firmware sends telemetry already in the board's display units — the app must NOT
  re-convert speed/range/trip.
- VESC Tool bridge and dashboard polling must not run simultaneously.
- The board's warmth is normal; not the fps work.

---

# (Archived) 2026-06-29 handoff — earlier OLED / unit-sync work

> Kept for reference. Superseded above where they overlap.

- Fixed LilyGO "COM exists but serial silent": `waitForBootReady()` could block
  forever with `demo OFF` + no VESC UART; `ui.cpp` now times out (~3.5s), shows
  `NO VESC - BLE READY`, continues. Verified COM5 answers `sys`/`cfg`.
- Generic targets `esp32s3_headless_usb`, `esp32s3_oled_i2c_usb`; dual console
  (USB CDC + Serial0/CH343); RGB status LED on GPIO48; OLED mini UI faces
  (speed/battery/volts/watts/safety); serial `hud` + `i2c` commands; OLED alert
  badges (no full-screen freeze); Amazon-style horizontal battery meter.
- BLE telemetry sends `mph` per frame (app labels stay synced to board unit toggles).
- Battery percent less twitchy near full under load; range-learn threshold 2 mi / 20 Wh
  (`RANGE_LEARN_MIN_DISTANCE_KM = 3.2`, `RANGE_LEARN_MIN_WH = 20.0`).
- Useful ride `scripts/_s0013_latest.txt`: 3.59 mi, 26.9 mph max, 37.9 A batt max,
  0 sag events; board learned 27.2 Wh/mi. Mid-ride faults were electrical, not firmware.
- Port detection:
  `Get-PnpDevice -PresentOnly | ? { $_.Class -in @('Ports','USB') -and ($_.FriendlyName -match 'COM|USB|Serial|CH343|ESP') }`
