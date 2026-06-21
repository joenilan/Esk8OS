# Esk8OS AI Implementation Handoff: VolosR Research + LilyGO T-Display-S3 Board Notes

## Purpose

This document is a working handoff for an AI coding agent to improve **Esk8OS** using ideas from VolosR’s ESP32 projects and the official LilyGO T-Display-S3 repository.

Primary project:

- Esk8OS: https://github.com/joenilan/Esk8OS

Research inputs:

- VolosR GitHub profile: https://github.com/VolosR/
- Official LilyGO T-Display-S3 repo: https://github.com/Xinyuan-LilyGO/T-Display-S3
- VolosR repositories of highest interest:
  - remoteVoltageMonitoring: https://github.com/VolosR/remoteVoltageMonitoring
  - XiaomiBLE: https://github.com/VolosR/XiaomiBLE
  - TPanelNetwork: https://github.com/VolosR/TPanelNetwork
  - tDisplayS3WeatherStation: https://github.com/VolosR/tDisplayS3WeatherStation
  - TTGOInternetStation: https://github.com/VolosR/TTGOInternetStation
  - eBikeRLCD: https://github.com/VolosR/eBikeRLCD
  - t-displayESPNOW: https://github.com/VolosR/t-displayESPNOW
  - ESP32asServer: https://github.com/VolosR/ESP32asServer
  - browserToEsp32: https://github.com/VolosR/browserToEsp32
  - NoHardCode: https://github.com/VolosR/NoHardCode

The goal is not to blindly copy code. The goal is to extract proven ESP32 patterns and adapt them cleanly into Esk8OS.

---

## Critical project framing

Esk8OS should remain a **VESC-centered electric skateboard dashboard and telemetry OS**.

It should not become the motor controller. The VESC remains the source of truth for ride control, motor current, battery current, braking, and safety limits. Esk8OS should improve:

- live telemetry display
- ride UI
- settings
- diagnostics
- sensor aggregation
- bridge/service mode
- optional wireless accessory nodes
- logging
- setup/maintenance experience

Do not add anything that can accidentally interfere with throttle, braking, or VESC control.

---

## Known Joe-specific hardware baseline

Current Joe build:

- Board: LilyGO T-Display-S3, non-touch version
- MCU: ESP32-S3 class board
- Display: 1.9-inch 170×320 ST7789-style display
- Target use: mounted electric skateboard dashboard
- VESC/ESC: Flipsky FSESC 6.7 Plus / VESC-compatible controller
- UART wiring memory:
  - Orange wire = ESP32 GPIO18 / U1 RXD, connected to FSESC COMM TX/SCL
  - Green wire = ESP32 GPIO17 / U1 TXD, connected to FSESC COMM RX/SDA
  - Red = 5V
  - Black = GND / negative
- Display/ESP32 connection: the LilyGO display is connected to the **VESC COMM port using UART**, not to the VESC PPM port.
- Remote/control input: the Flipsky VX1 receiver is handled separately through the VESC PPM input path. Esk8OS should not try to replace the VX1, read the VX1, or modify PPM behavior.

### Current VESC/display connection topology

This distinction is important for all future code changes:

```text
Flipsky VX1 receiver  ->  VESC PPM/input port  ->  rider throttle/brake control
LilyGO T-Display-S3   ->  VESC COMM UART port   ->  telemetry / bridge / config access
```

Esk8OS is a COMM-port UART telemetry and service device. It is **not** in the PPM throttle path.

Wiring from the LilyGO perspective:

```text
ESP32 GPIO18 / U1 RXD  <-  VESC COMM TX/SCL   orange wire
ESP32 GPIO17 / U1 TXD  ->  VESC COMM RX/SDA   green wire
ESP32 5V/VBUS          <-  VESC COMM 5V       red wire
ESP32 GND              <-> VESC COMM GND/-    black wire
```

Notes for AI agents:

- Do not confuse VESC-side TX/RX labels with ESP32-side TX/RX labels. ESP RX receives from VESC TX. ESP TX sends to VESC RX.
- Some Flipsky/VESC COMM headers label the same physical pins as `TX/SCL` and `RX/SDA` because the connector can support UART/I2C-style functions. For Esk8OS, use UART.
- Bridge mode, telemetry polling, and future diagnostics all share this same COMM UART.
- When bridge mode is active, bridge mode owns the COMM UART and normal dashboard telemetry should pause or clearly enter bridge/service state.
- PPM is for the VX1 remote and should be treated as outside Esk8OS.

Important: Joe’s board is **not** the touchscreen model. However, Esk8OS should be structured so a future user with the **T-Display-S3 Touch** variant can enable touch support through build flags or board config.

---

## Official LilyGO T-Display-S3 facts to respect

From the official LilyGO repo:

- Supported products include:
  - T-Display-S3
  - T-Display-S3-Touch
  - T-Display-S3-MIDI
- T-Display-S3 and T-Display-S3 Touch are listed as ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM, 170×320, 1.9-inch display.
- The repo contains useful examples:
  - `GetBatteryVoltage`
  - `I2CScan`
  - `SerialExample`
  - `touch_test`
  - `ota`
  - `lv_demos`
  - BLE sender/receiver examples
  - USB HID examples
  - ULP ADC/count examples
  - VolosR examples such as `PCBClock` and `PokerS3`
- The official FAQ says GPIO15 must be set HIGH when powered from battery to turn on display/backlight power.
- The official FAQ says external/battery-powered builds should disable USB CDC on boot because the board may wait for USB access at startup.
- TFT_eSPI can be fragile across Arduino-ESP32 versions. Esk8OS should prefer its current LovyanGFX-style display approach unless there is a strong reason to change.

Mandatory board-init rule:

```cpp
pinMode(15, OUTPUT);
digitalWrite(15, HIGH);
```

This belongs in a board hardware abstraction layer, not scattered inside UI or app logic.

---

## High-level conclusion from VolosR research

VolosR has many ESP32 projects, but the most useful ideas for Esk8OS are **around the dashboard core**, not inside the VESC control path.

Best VolosR patterns to adapt:

1. **ESP-NOW auxiliary sensor nodes** from `remoteVoltageMonitoring`
2. **BLE sensor ingestion** from `XiaomiBLE`
3. **Touch/network diagnostics/admin panel ideas** from `TPanelNetwork`
4. **Wi-Fi onboarding and board backlight/power patterns** from `tDisplayS3WeatherStation`
5. **Explicit Wi-Fi/session lifecycle management** from `TTGOInternetStation`
6. **Display backend abstraction / secondary display ideas** from `eBikeRLCD`
7. **Browser-based config/admin possibilities** from `ESP32asServer` and `browserToEsp32`
8. **Board portability mindset** from `NoHardCode`

The most important architectural lesson:

> Esk8OS should grow into a modular sensor hub + ride dashboard, not a single giant Arduino sketch.

---

## Licensing caution

Many VolosR repos appear to be demo/tutorial projects and may not expose a clear license in the inspected pages. Treat VolosR repos as **reference implementations** unless a license explicitly allows reuse.

Implementation rule for the AI agent:

- Do not copy-paste VolosR code into Esk8OS.
- Re-implement patterns cleanly.
- If code is copied, first verify the license and preserve attribution.
- Prefer writing original modules inspired by the observed architecture.

---

## Non-negotiable engineering constraints

The AI agent must follow these constraints:

1. Preserve the current working Esk8OS behavior unless a task explicitly changes it.
2. Do not rewrite the entire UI.
3. Do not convert the ride dashboard to LVGL.
4. Do not enable touch by default.
5. Do not block boot waiting for USB in ride builds.
6. Do not make Wi-Fi, BLE, or ESP-NOW ride-critical.
7. Do not make auxiliary sensors authoritative over VESC data.
8. Do not break VESC COMM UART bridge mode.
9. Do not move Joe’s COMM UART pins unless explicitly requested.
10. Do not touch, repurpose, or assume control of the VESC PPM input path.
11. Do not remove existing settings without a migration path.
12. Do not add speculative features without a build flag or setting.
13. Compile and test after each phase.

---

## Target architecture

Refactor Esk8OS toward this module layout.

```text
src/
  main.cpp

  app/
    AppState.h
    AppState.cpp
    Scheduler.h
    Scheduler.cpp
    Modes.h

  board/
    BoardConfig.h
    BoardLilyGoTDisplayS3.h
    BoardLilyGoTDisplayS3.cpp
    Backlight.h
    Backlight.cpp
    Buttons.h
    Buttons.cpp
    TouchInput.h
    TouchInput.cpp

  config/
    Esk8OSConfig.h
    BuildInfo.h
    Settings.h
    Settings.cpp
    SettingsMigration.h
    SettingsMigration.cpp

  telemetry/
    TelemetryModel.h
    TelemetryModel.cpp
    Metric.h
    VescTelemetry.h
    AuxTelemetry.h
    TelemetryAggregator.h
    TelemetryAggregator.cpp

  transports/
    VescUartTransport.h
    VescUartTransport.cpp
    WifiBridgeTransport.h
    WifiBridgeTransport.cpp
    EspNowTransport.h
    EspNowTransport.cpp
    BleSensorTransport.h
    BleSensorTransport.cpp

  sensors/
    AuxVoltageSensor.h
    AuxVoltageSensor.cpp
    AuxTemperatureSensor.h
    AuxTemperatureSensor.cpp

  services/
    NetworkService.h
    NetworkService.cpp
    BridgeService.h
    BridgeService.cpp
    DiagnosticsService.h
    DiagnosticsService.cpp
    OtaService.h
    OtaService.cpp

  ui/
    UiRouter.h
    UiRouter.cpp
    DashboardPages.h
    DashboardPages.cpp
    ServicePages.h
    ServicePages.cpp
    Widgets.h
    Widgets.cpp
    Theme.h
    Theme.cpp

  logging/
    RideLog.h
    RideLog.cpp
    EventLog.h
    EventLog.cpp

  util/
    TimeUtil.h
    Crc32.h
    RingBuffer.h
```

This does not need to happen all at once. The first refactor should be mechanical and behavior-preserving.

---

## Build environments

Create separate PlatformIO environments.

### Required environments

```ini
[env:tdisplay_s3_debug_usb]
; Developer build
; USB CDC enabled
; Serial logs visible over USB
; Safe for desk testing

[env:tdisplay_s3_ride_release]
; Ride build
; USB CDC disabled so the board does not wait for USB on battery/external power
; Wi-Fi/BLE optional
; Stable startup is more important than serial logs

[env:tdisplay_s3_touch_debug]
; Touch-capable build
; Only for T-Display-S3 Touch users
; Touch code enabled behind compile flag

[env:tdisplay_s3_touch_ride]
; Touch-capable ride build
; Touch support enabled but USB CDC disabled
```

### Suggested build flags

Debug USB build:

```ini
build_flags =
  -DESK8OS_BOARD_TDISPLAY_S3=1
  -DESK8OS_HAS_TOUCH=0
  -DARDUINO_USB_CDC_ON_BOOT=1
```

Ride release build:

```ini
build_flags =
  -DESK8OS_BOARD_TDISPLAY_S3=1
  -DESK8OS_HAS_TOUCH=0
  -UARDUINO_USB_CDC_ON_BOOT
```

Touch debug build:

```ini
build_flags =
  -DESK8OS_BOARD_TDISPLAY_S3=1
  -DESK8OS_HAS_TOUCH=1
  -DARDUINO_USB_CDC_ON_BOOT=1
```

Touch ride build:

```ini
build_flags =
  -DESK8OS_BOARD_TDISPLAY_S3=1
  -DESK8OS_HAS_TOUCH=1
  -UARDUINO_USB_CDC_ON_BOOT
```

Do not assume touch hardware is present unless `ESK8OS_HAS_TOUCH=1`.

---

## Board HAL requirements

Create a board layer that owns:

- display power
- GPIO15 power enable
- backlight brightness
- button reads
- optional touch input
- optional onboard battery ADC
- board self-test data
- USB/debug behavior awareness
- pin definitions

Suggested API:

```cpp
namespace Esk8OS::Board {
  struct BoardStatus {
    bool displayPowerEnabled;
    bool touchPresent;
    bool batteryAdcValid;
    float boardBatteryVolts;
    uint8_t brightness;
  };

  void begin();
  void enableDisplayPower();
  void setBacklight(uint8_t brightness);
  uint8_t getBacklight();
  BoardStatus readStatus();

  bool buttonA();
  bool buttonB();

#if ESK8OS_HAS_TOUCH
  bool touchAvailable();
  bool readTouch(uint16_t& x, uint16_t& y);
#endif
}
```

Implementation note:

```cpp
void enableDisplayPower() {
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
}
```

Call this early in `setup()` before display initialization.

---

## Touch support strategy

Joe’s current board is non-touch, so touch support must be optional.

Touch must be integrated as an input backend, not as a UI rewrite.

### Required behavior

- Non-touch build:
  - no touch library required
  - no touch initialization
  - buttons remain the primary input
  - no crash or boot delay from missing touch hardware

- Touch build:
  - touch driver initialized from official LilyGO example/pin map
  - input events flow into the same UI action system as buttons
  - touch is used for service/settings screens first
  - ride HUD remains glanceable and button-compatible

### Suggested input abstraction

```cpp
enum class InputEventType {
  None,
  ButtonShort,
  ButtonLong,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
  Tap,
  LongPress
};

struct InputEvent {
  InputEventType type;
  uint16_t x;
  uint16_t y;
  uint32_t timestampMs;
};
```

Button and touch drivers should both emit `InputEvent`.

### Touch-first features to add later

Do not start with touch on the ride HUD. Start with:

- settings page
- diagnostics page
- Wi-Fi setup page
- sensor pairing page
- log viewer
- OTA/update page

This follows the useful part of VolosR’s `TPanelNetwork`: touch is best used for admin/config workflows, not necessarily the primary moving/riding display.

---

## Display/UI strategy

Keep the current Esk8OS ride dashboard renderer.

Do not replace the primary ride UI with LVGL. LVGL can be useful for:

- touch settings
- service mode
- admin panel
- future browser-like UI experiments
- separate maintenance firmware target

But the ride HUD should stay fast, simple, and predictable.

UI goals:

- large readable speed
- clean battery display
- no duplicate battery bars unless intentionally configured
- no random layout drift
- no tiny hidden warnings
- stale/invalid sensor values must be visually obvious
- every metric should show source/state internally, even if the main page displays it simply

---

## Telemetry model

The current code should move toward a source-aware telemetry model.

Each metric should know:

- value
- validity
- source
- timestamp
- stale state
- quality/confidence

Suggested types:

```cpp
enum class TelemetrySource : uint8_t {
  None,
  VescUart,
  EspNowNode,
  BleSensor,
  BoardAdc,
  ManualSetting,
  Derived
};

template <typename T>
struct Metric {
  T value{};
  bool valid = false;
  bool stale = true;
  TelemetrySource source = TelemetrySource::None;
  uint32_t updatedMs = 0;
  uint8_t quality = 0;
};
```

Core telemetry object:

```cpp
struct TelemetryModel {
  Metric<float> speedMph;
  Metric<float> dutyCycle;
  Metric<float> motorCurrentA;
  Metric<float> batteryCurrentA;
  Metric<float> packVoltageV;
  Metric<float> vescMosfetTempC;
  Metric<float> motorTempC;

  Metric<float> auxPackVoltageV;
  Metric<float> auxPackTempC;
  Metric<float> enclosureTempC;
  Metric<float> boardBatteryV;

  Metric<uint32_t> faultCode;
  Metric<float> tripMiles;
  Metric<float> estimatedRangeMiles;
};
```

Rules:

- VESC COMM UART is authoritative for VESC data.
- Board ADC is only board battery/charger state, not esk8 pack voltage.
- ESP-NOW/BLE auxiliary sensors can improve warnings and logging.
- Auxiliary sensors should never override VESC safety/control.
- PPM/VX1 throttle and braking control is outside Esk8OS.
- Derived values must record their source as `Derived`.

---

## ESP-NOW auxiliary sensor framework

Inspired by VolosR `remoteVoltageMonitoring`.

Purpose:

- add wireless accessory nodes
- measure enclosure temperature
- optionally measure independent pack/enclosure voltage
- support future GPS or BMS nodes
- keep VESC COMM UART clean

### Recommended first node

Build a simple ESP32 aux node with:

- temperature sensor
- optional ADS1115 voltage input
- ESP-NOW sender
- unique node ID
- sequence number
- packet version
- CRC
- stale timeout on receiver

### Packet design

Do not send only a raw float. Use versioned packets.

```cpp
#pragma pack(push, 1)
struct AuxTelemetryPacketV1 {
  uint16_t magic;          // 0xE805
  uint8_t version;         // 1
  uint8_t nodeType;        // battery, enclosure, gps, etc.
  uint32_t nodeId;
  uint32_t seq;
  uint32_t uptimeMs;

  int32_t packMilliVolts;  // -1 if unavailable
  int16_t tempCx10;        // -32768 if unavailable
  int16_t humidityX10;     // -1 if unavailable

  uint16_t flags;
  uint32_t crc32;
};
#pragma pack(pop)
```

### Receiver behavior

- Accept only packets with correct magic/version/CRC.
- Track `lastSeq`, `lastSeenMs`, and packet loss.
- Mark node stale after configurable timeout, e.g. 3–5 seconds.
- Show stale warning on service page.
- Do not spam ride HUD unless the value affects safety.
- Log sensor loss/recovery events.

### Suggested UI warnings

- `AUX TEMP STALE`
- `AUX VOLT STALE`
- `PACK TEMP HIGH`
- `ENCLOSURE HOT`
- `AUX NODE LOST`

### Settings

Add settings for:

- ESP-NOW enabled
- allowed peer MACs
- node display names
- stale timeout
- temp warning threshold
- whether aux pack voltage is shown on HUD

---

## BLE sensor backend

Inspired by VolosR `XiaomiBLE`.

Use BLE first for auxiliary sensors, not VESC-over-BLE.

Possible BLE devices:

- temperature/humidity sensors
- battery compartment sensors
- rider/wearable sensor later
- future BMS BLE later if protocol is known
- future VESC BLE only after UART path is stable

### Architecture

```cpp
class BleSensorTransport {
public:
  void begin();
  void loop();
  bool enabled() const;
  void setEnabled(bool enabled);
};
```

Device config:

```cpp
struct BleSensorConfig {
  char name[24];
  char mac[18];
  char serviceUuid[40];
  char characteristicUuid[40];
  uint8_t parserType;
  bool enabled;
};
```

Callback rule:

- BLE notifications should update sensor state only.
- The UI should not render directly from callbacks.
- The aggregator should consume BLE values during the main loop.

### Initial parser types

- Generic temperature sensor
- Xiaomi-style temp/humidity sensor
- Raw custom characteristic
- Disabled/unknown

### BLE caution

BLE can be unstable on embedded dashboards if it blocks. The BLE backend must:

- reconnect gently
- avoid long blocking scans during ride mode
- expose stale state
- be disable-able from settings
- never interfere with VESC COMM UART timing

---

## Wi-Fi bridge and network lifecycle

Esk8OS already has or is intended to have a Wi-Fi AP/TCP bridge so VESC Tool can own the VESC COMM UART temporarily.

Improve this with a formal network owner state machine.

```cpp
enum class NetworkOwner {
  None,
  BridgeMode,
  WifiSetup,
  OtaUpdate,
  WebAdmin,
  TelemetryUpload
};
```

Rules:

- Only one network owner at a time.
- Bridge mode owns the COMM UART while active.
- Dashboard telemetry pauses or enters read-only/service status during bridge mode.
- OTA must not start during bridge mode.
- Wi-Fi setup must not start while riding.
- BLE/ESP-NOW can continue only if they do not destabilize the network stack.
- Service screen must clearly show current network owner.

From VolosR `TTGOInternetStation`, borrow the idea of explicit connect/retry/teardown lifecycle, not the audio code.

---

## Wi-Fi provisioning and service mode

Inspired by VolosR `tDisplayS3WeatherStation` and `TPanelNetwork`.

Esk8OS should eventually have a service mode with:

- Wi-Fi AP bridge state
- SSID
- IP address
- RSSI
- MAC address
- connected client count
- bridge throughput
- COMM UART RX/TX bytes
- VESC heartbeat freshness
- firmware version/build info
- PSRAM/heap/free memory
- board battery if present
- touch present/not present
- ESP-NOW peers
- BLE sensor states
- last fault/event log

WiFiManager-style provisioning can be useful, but only outside live ride mode.

Recommended modes:

```cpp
enum class SystemMode {
  Dashboard,
  Settings,
  Bridge,
  WifiSetup,
  Diagnostics,
  SensorPairing,
  OtaUpdate
};
```

---

## Settings and persistence

Move settings into clear namespaces.

Suggested Preferences namespaces:

```text
esk8os_app
esk8os_board
esk8os_wheel
esk8os_battery
esk8os_display
esk8os_bridge
esk8os_espnow
esk8os_ble
esk8os_touch
esk8os_logs
```

Every namespace should have a version.

```cpp
struct SettingsHeader {
  uint16_t schemaVersion;
  uint16_t crc;
};
```

Add a migration system so future settings do not randomly reset existing users.

### Settings to expose eventually

Board/display:

- brightness
- screen timeout
- orientation
- theme
- large HUD mode

Ride math:

- wheel diameter
- pulley teeth / gear ratio if relevant
- motor poles if needed
- battery series count
- battery capacity Ah
- low battery warning thresholds

Bridge:

- bridge enabled
- AP SSID
- AP password
- TCP port
- auto-timeout

Aux sensors:

- ESP-NOW enabled
- BLE enabled
- node list
- stale timeouts
- warning thresholds

Touch:

- touch enabled
- calibration if needed
- gesture sensitivity
- touch lock while riding

---

## Logging and event history

Add a compact event log separate from ride logs.

Log events such as:

- boot
- display power enabled
- VESC connected
- VESC lost
- bridge started
- bridge stopped
- ESP-NOW node joined/lost
- BLE sensor connected/lost
- high temp warning
- low voltage warning
- VESC fault code
- settings migration
- OTA attempt/success/failure

Suggested event type:

```cpp
enum class EventType : uint8_t {
  Boot,
  VescConnected,
  VescLost,
  BridgeStarted,
  BridgeStopped,
  EspNowNodeSeen,
  EspNowNodeLost,
  BleSensorConnected,
  BleSensorLost,
  WarningRaised,
  WarningCleared,
  SettingsMigrated,
  OtaStarted,
  OtaSucceeded,
  OtaFailed
};
```

Keep this small so it works on-device.

---

## OTA strategy

The official LilyGO repo includes an OTA example. Add OTA later, not first.

OTA requirements:

- disabled by default
- only accessible in service mode
- no OTA while bridge mode is active
- no OTA while moving/riding
- show progress on display
- rollback/factory recovery plan preferred
- confirm target board/build compatibility before flashing
- never block boot if network missing

Possible OTA modes:

1. Local web upload
2. Pull from a manifest URL
3. USB-only update remains default safe path

Start with local web upload only if needed.

---

## Optional browser/admin interface

VolosR `ESP32asServer` and `browserToEsp32` suggest browser-based control/config patterns.

Possible future Esk8OS browser pages:

- live telemetry view
- settings editor
- VESC bridge start/stop
- log download
- firmware info
- sensor pairing
- OTA upload

This should be service-mode only and password protected.

Do not run a browser admin server during normal riding unless explicitly enabled.

---

## Secondary display / accessory display ideas

VolosR `eBikeRLCD` is interesting because it shows custom display adaptation and gauge-cluster logic.

Do not implement this unless there is a real hardware target.

Possible future use:

- tiny remote display
- low-power always-on status panel
- handlebar pod
- fallback fault indicator
- enclosure-mounted diagnostic display

The architecture should make this possible by separating UI rendering from telemetry state, but it is not a priority now.

---

## Prioritized roadmap

### Phase 0 — Baseline safety snapshot

Goal: know exactly what works before refactoring.

Tasks:

1. Confirm current project builds.
2. Save current known-good firmware binary.
3. Record current PlatformIO environment.
4. Document current pages and button behavior.
5. Document current COMM UART pins and confirm this is the VESC COMM port, not PPM.
6. Document current VESC bridge behavior.
7. Add a `CHANGELOG.md` if missing.
8. Add a `docs/known_good.md`.

Acceptance criteria:

- Existing behavior is documented.
- Current firmware can be rebuilt.
- No functional changes yet.

---

### Phase 1 — Board HAL + build environments

Goal: fix board-specific assumptions safely.

Tasks:

1. Add board config files.
2. Move GPIO15 display power enable into board HAL.
3. Add backlight setter/getter.
4. Add debug USB and ride release PlatformIO environments.
5. Add non-touch and touch compile flags.
6. Add board self-test page or diagnostic section.
7. Ensure ride release does not block on USB CDC.
8. Ensure non-touch build does not include touch dependencies.

Acceptance criteria:

- Non-touch debug build compiles.
- Non-touch ride build compiles.
- Touch build compiles or is stubbed safely.
- Display powers reliably from external/battery power.
- Existing UI still works.

---

### Phase 2 — Modular refactor without behavior changes

Goal: split the monolith.

Tasks:

1. Extract settings module.
2. Extract board module.
3. Extract UI page rendering module.
4. Extract VESC COMM UART module.
5. Extract Wi-Fi bridge module.
6. Extract telemetry model.
7. Keep functionally identical behavior.

Acceptance criteria:

- Project compiles.
- Existing dashboard pages still work.
- Existing settings still load.
- VESC COMM UART still works.
- Bridge mode still works.
- Diff is mostly code movement, not feature changes.

---

### Phase 3 — Source-aware telemetry aggregator

Goal: prepare for VESC + auxiliary sensors.

Tasks:

1. Add `Metric<T>` type.
2. Add `TelemetrySource`.
3. Add `TelemetryModel`.
4. Update VESC telemetry path to write source-aware metrics.
5. Add stale detection.
6. Add display fallback for stale/invalid metrics.
7. Add source/state data to diagnostics page.

Acceptance criteria:

- Existing VESC values display correctly.
- Lost VESC data becomes stale instead of fake-valid.
- UI does not crash on invalid metrics.
- Diagnostics show source and age for key metrics.

---

### Phase 4 — ESP-NOW auxiliary node support

Goal: add wireless aux sensor path.

Tasks:

1. Add `EspNowTransport`.
2. Define `AuxTelemetryPacketV1`.
3. Add CRC validation.
4. Add peer/node registry.
5. Add stale timeout.
6. Add diagnostics page section for nodes.
7. Add warnings for lost/high-temp nodes.
8. Build a minimal sender example in `examples/espnow_aux_node`.

Acceptance criteria:

- Receiver compiles with ESP-NOW disabled by default.
- Enabling ESP-NOW does not break VESC COMM UART.
- Test packet updates auxiliary telemetry.
- Stale/lost node warning works.
- Invalid CRC packet is rejected.

---

### Phase 5 — BLE sensor backend

Goal: add optional BLE peripheral ingestion.

Tasks:

1. Add `BleSensorTransport`.
2. Add BLE enable setting.
3. Add BLE sensor config list.
4. Add parser scaffold.
5. Add at least one test parser.
6. Add diagnostics page section.
7. Add reconnect/stale behavior.
8. Keep BLE disabled by default.

Acceptance criteria:

- BLE-disabled build behaves exactly like before.
- BLE-enabled build does not block dashboard loop.
- A test BLE notification can update a metric.
- Lost BLE sensor becomes stale.

---

### Phase 6 — Service mode network diagnostics

Goal: improve bridge/setup troubleshooting.

Tasks:

1. Add `NetworkOwner`.
2. Add network service state machine.
3. Add diagnostics for IP/RSSI/MAC/client count/bridge bytes.
4. Add bridge timeout.
5. Add bridge start/stop confirmation.
6. Add clear UART ownership rules.
7. Add service page UI.

Acceptance criteria:

- Bridge mode clearly owns UART.
- Dashboard does not fight bridge mode.
- Network diagnostics are visible.
- Bridge exits cleanly.

---

### Phase 7 — Optional touch support

Goal: support T-Display-S3 Touch users without affecting Joe’s non-touch board.

Tasks:

1. Add touch driver only behind `ESK8OS_HAS_TOUCH`.
2. Use official LilyGO touch example/pin map.
3. Add touch presence/status to diagnostics.
4. Add tap/swipe input events.
5. Make settings/service pages touch-friendly.
6. Keep button navigation working.
7. Add touch lockout option for riding.

Acceptance criteria:

- Non-touch build unaffected.
- Touch build compiles.
- Touch input does not crash when absent.
- Settings/service pages accept touch input.
- Ride HUD remains button-compatible.

---

### Phase 8 — OTA and browser admin

Goal: improve maintainability after core architecture is stable.

Tasks:

1. Inspect official LilyGO OTA example.
2. Add OTA service disabled by default.
3. Add local OTA upload or manifest-based OTA.
4. Add progress display.
5. Add firmware compatibility check.
6. Add browser admin only in service mode.
7. Add log download.

Acceptance criteria:

- OTA cannot start while riding.
- OTA cannot start while bridge owns network/UART.
- Failed network does not block boot.
- Firmware version/build is visible.

---

## AI agent task template

Use this format for each coding pass.

```text
Task: <short name>

Goal:
<one clear goal>

Constraints:
- Preserve existing behavior unless stated.
- Do not rewrite unrelated code.
- Keep non-touch build as baseline.
- Keep VESC COMM UART pins unchanged.
- Keep bridge mode working.
- Compile after changes.

Files likely involved:
- <list>

Implementation steps:
1. <step>
2. <step>
3. <step>

Acceptance criteria:
- <criteria>
- <criteria>

Do not:
- <specific risks>
```

---

## First task to give an AI agent

```text
Task: Add LilyGO T-Display-S3 board HAL and safe build environments

Goal:
Create a board abstraction layer for the LilyGO T-Display-S3 and split PlatformIO into debug USB and ride release environments. This should be a behavior-preserving change except for moving board-specific setup into the new board layer.

Constraints:
- Joe’s current board is non-touch.
- Do not enable touch by default.
- Do not change VESC COMM UART pins.
- Do not rewrite the dashboard UI.
- Do not change VESC COMM bridge behavior.
- Ride release must not block waiting for USB CDC.
- Keep current LovyanGFX display strategy.

Implementation steps:
1. Create `src/board/BoardLilyGoTDisplayS3.h/.cpp`.
2. Move GPIO15 display/backlight power enable into `Board::enableDisplayPower()`.
3. Call board power initialization early in `setup()`.
4. Add `Board::setBacklight(uint8_t)` and `Board::getBacklight()`.
5. Add `ESK8OS_HAS_TOUCH` compile flag defaulting to 0.
6. Add PlatformIO envs:
   - `tdisplay_s3_debug_usb`
   - `tdisplay_s3_ride_release`
   - optional/stub `tdisplay_s3_touch_debug`
7. Ensure debug USB enables USB CDC.
8. Ensure ride release disables USB CDC.
9. Build both non-touch environments.

Acceptance criteria:
- Existing dashboard still renders.
- Existing buttons still work.
- Existing VESC COMM UART still works.
- Existing bridge mode still works.
- Display powers reliably when not connected to USB.
- Non-touch build has no touch dependency.
```

---

## Second task to give an AI agent

```text
Task: Add source-aware TelemetryModel

Goal:
Introduce a `TelemetryModel` with validity, stale state, source, and timestamp for each metric. Start by routing existing VESC values into the model without changing page visuals.

Constraints:
- Do not change dashboard layout in this task.
- Do not change VESC packet parsing behavior.
- Do not add ESP-NOW or BLE yet.
- Do not fake missing values as valid.

Implementation steps:
1. Create `src/telemetry/Metric.h`.
2. Create `src/telemetry/TelemetryModel.h/.cpp`.
3. Add `TelemetrySource` enum.
4. Update VESC update path to write metrics with `TelemetrySource::VescUart`.
5. Add stale timeout helpers.
6. Add basic diagnostics output for metric age/source.
7. Keep existing display fields reading equivalent values.

Acceptance criteria:
- Existing pages display the same values as before.
- Lost VESC telemetry becomes stale internally.
- No crashes if telemetry is invalid.
- Code compiles in debug and ride builds.
```

---

## Third task to give an AI agent

```text
Task: Add ESP-NOW auxiliary telemetry skeleton

Goal:
Add an optional ESP-NOW receiver for future aux sensor nodes, inspired by VolosR `remoteVoltageMonitoring`, but implemented as original Esk8OS code.

Constraints:
- Disabled by default.
- No copied VolosR code.
- Must not affect VESC COMM UART timing when disabled.
- Must not control the VESC.
- Auxiliary values are warnings/logging/display only.

Implementation steps:
1. Create `src/transports/EspNowTransport.h/.cpp`.
2. Define `AuxTelemetryPacketV1`.
3. Add packet validation: magic, version, size, CRC.
4. Add node tracking: node ID, last seen, sequence, stale state.
5. Add compile flag `ESK8OS_ENABLE_ESPNOW`.
6. Add settings placeholders for ESP-NOW enable and stale timeout.
7. Add diagnostics page data model for nodes.
8. Add `examples/espnow_aux_node` sender sketch.

Acceptance criteria:
- Default build behaves unchanged.
- ESP-NOW build compiles.
- Valid test packet updates aux telemetry.
- Invalid packet is rejected.
- Stale node is detected.
```

---

## Features to consider later

Good ideas, but not first-pass priorities:

- GPS module and ride map export
- phone companion page
- browser dashboard
- OTA firmware update
- BMS BLE integration
- VESC BLE backend
- SD card ride logs
- web log download
- secondary low-power display
- ESP-NOW remote button pod
- ambient light sensor for auto brightness
- enclosure fan control
- LED strip effects controlled by ride state

---

## Mistakes to avoid

Do not do these:

- Do not turn Esk8OS into a throttle/remote controller.
- Do not let BLE or ESP-NOW affect braking/throttle.
- Do not block the main loop with Wi-Fi/BLE scans.
- Do not assume every board has touch.
- Do not assume board ADC equals skateboard pack voltage.
- Do not leave GPIO15 setup buried inside random display code.
- Do not keep adding features into one giant `main.cpp`.
- Do not add settings without schema versioning.
- Do not add OTA before network ownership rules exist.
- Do not copy unlicensed code from VolosR.
- Do not downgrade display architecture just because examples use TFT_eSPI.
- Do not make the ride HUD cluttered with every possible metric.
- Do not make warnings tiny or invisible.
- Do not break USB debugging for development builds.
- Do not leave ride builds waiting for USB CDC.

---

## Final recommended near-term plan

The next real work should be:

1. Board HAL + PlatformIO env split
2. Modular refactor out of `main.cpp`
3. Source-aware telemetry model
4. Diagnostics/service page cleanup
5. ESP-NOW aux sensor skeleton
6. BLE sensor skeleton
7. Optional touch support for T-Display-S3 Touch users
8. OTA/browser admin later

The most useful VolosR-inspired feature to build first is **ESP-NOW auxiliary telemetry**, but only after the board HAL and telemetry model are cleaned up.

The most useful LilyGO official-repo fix to build first is **GPIO15 display power + USB CDC ride/debug env split**.

