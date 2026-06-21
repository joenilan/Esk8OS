# ESK8OS Companion App - BLE API Specification

This document details the custom Bluetooth Low Energy (BLE) protocol that the ESP32 firmware will expose. You can hand this document to any future agent or Android developer to instantly build a companion app that natively syncs with your longboard.

## 1. Overview
Instead of multiplexing the raw VESC UART protocol, the ESP32 acts as the single "master" querying the VESC. The ESP32 parses the data, runs the math (speed, range, battery limits), and broadcasts the final display-ready values over a custom BLE service using lightweight JSON. 

The Android app acts as a read/write client to this custom service.

## 2. BLE UUIDs

**Base Service UUID:** `5043697A-0000-4682-93CB-33BB0A149F7E`

| Characteristic | UUID | Properties | Purpose |
| :--- | :--- | :--- | :--- |
| **Telemetry** | `5043697A-0001-4682-93CB-33BB0A149F7E` | `NOTIFY` | High-frequency ride data (Speed, Battery, etc) |
| **Settings** | `5043697A-0002-4682-93CB-33BB0A149F7E` | `READ`, `WRITE` | Board configuration (Theme, Wheel size, MPH/KMH) |
| **Command** | `5043697A-0003-4682-93CB-33BB0A149F7E` | `WRITE` | Triggers for actions (Trip Reset, Change Page) |

---

## 3. Telemetry (Characteristic `0001`)
The ESP32 pushes a JSON payload at **5Hz (every 200ms)** while the board is on. 
The companion app simply subscribes to notifications on this characteristic and parses the JSON to update its UI.

**Example Payload:**
```json
{
  "spd": 24.5,       // Current speed (in whatever unit the board is configured for)
  "bat": 85,         // Battery percentage (0-100)
  "v": 45.2,         // Battery voltage
  "w": 650,          // Current power in Watts
  "mtr_t": 45,       // Motor temp (Celsius)
  "esc_t": 38,       // ESC temp (Celsius)
  "rng": 12.4,       // Estimated remaining range
  "max_s": 40.1,     // Session Max Speed
  "wh": 120          // Session Watt-Hours used
}
```

---

## 4. Settings (Characteristic `0002`)
Allows the companion app to remotely configure the ESP32 without touching the physical buttons.

### Read
When the Android app connects, it reads this characteristic to get the board's current configuration.
```json
{
  "mph": true,
  "theme": "cam",
  "poles": 14,
  "wheel": 105,
  "gear": 2.4,
  "bat_s": 12
}
```

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
| `"TRIP_RESET"` | Resets the current session/trip distance and max metrics. |
| `"PAGE_NEXT"` | Swipes the physical ESP32 display to the next page. |
| `"PAGE_PREV"` | Swipes the physical ESP32 display to the previous page. |
| `"BRIDGE_MODE"` | Halts telemetry and forces the board into VESC Tool Bridge Mode. |
| `"WIFI_EXPORT_START"`| Turns on the ESP32's WiFi AP and HTTP server for bulk log downloading. |
| `"REBOOT"` | Reboots the ESP32 display. |

## 6. Hybrid WiFi Log Transfer (Bulk Data)
Because the ESP32 logs 1 line of CSV per second, historical ride logs can easily exceed 500 KB. Attempting to download files this large over BLE is mathematically slow and prone to timeout errors. 

To download historical logs, the Android app should use a **Hybrid Transfer**:
1. The app writes `"WIFI_EXPORT_START"` to the Command characteristic (`0003`).
2. The ESP32 immediately spins up its `ESK8-BRIDGE` WiFi Access Point.
3. The Android app prompts the user to join the `ESK8-BRIDGE` network (Password: `esk8bridge`).
4. The Android app hits the ESP32's internal HTTP Web Server (`http://192.168.4.1/`) to download the raw CSV files over fast TCP.
5. Once downloaded, the user disconnects from the WiFi, and the board seamlessly resumes BLE telemetry.

## 7. Implementation Notes for Android Devs
- **MTU Negotiation:** Ensure the Android BLE client requests a high MTU (e.g., `512` bytes) upon connection. JSON payloads are small, but they will exceed the default `20` byte BLE limit. **The telemetry notify is sent as one JSON object** — without a negotiated MTU it will silently truncate.
- **Connection Loss:** If the Android app disconnects, the ESP32 will automatically resume advertising in the background.

## 8. Firmware Implementation Status (as built)
The ESP32 side of this spec is implemented in `src/services/companion_ble.cpp` (NimBLE + ArduinoJson). Notes on what is live and where reality differs from the draft above:

- **Always-on service.** The companion service initializes in `setup()` and advertises 100% of the time. The device name is **`ESK8-BLE`** (the companion service UUID is in the primary advertisement; the name + VESC-Tool NUS UUID are in the scan response). Use **active scanning** and filter by the service UUID `5043697A-0000-…`.
- **Co-existence with VESC Bridge mode.** The VESC-Tool NUS bridge shares the *same* NimBLE server. Entering Bridge mode (physically, or via the `BRIDGE_MODE` command) does not tear down the companion service — but **telemetry notifications pause** while bridging (consistent with §6), and queued Settings/Command writes are applied once the dashboard resumes.
- **Settings writes (§4):** `mph` (bool), `theme` (string, case-insensitive), and `bat_s` (int, clamped 6–14) are honored and persisted to NVS. `poles`, `wheel`, and `gear` are **read-only** — they are derived from the selected wheel preset, not independently settable. To change them, write **`profile`** (int index) to select a preset. `gear` is reported as the firmware's motor:wheel pulley ratio.
- **`WIFI_EXPORT_START` (§5/§6):** currently routes through full VESC Bridge mode, which brings up the `ESK8-BRIDGE` AP + the `http://192.168.4.1/` log server exactly as §6 describes.
- **Settings/Command writes** are processed on the firmware's UI thread (BLE callbacks only enqueue), so display repaints triggered by a write are race-free.
